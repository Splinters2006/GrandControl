#!/usr/bin/env python3
"""
GrandControl relay server.

Run this on your VPS or on your own port-forwarded home machine.

Protocol summary:
  Agent:  AGENT <device_id>          (no token – relay is private/firewalled)
  Admin:  ADMIN <admin_token>

The relay never executes commands. It only forwards commands from an
admin client to a connected agent.

Environment variables:
  GC_RELAY_HOST   bind address  (default 0.0.0.0)
  GC_RELAY_PORT   port          (default 8443)
  GC_ADMIN_TOKEN  admin secret  (MUST be changed from the default)
"""

import asyncio
import base64
import json
import os
import secrets
from dataclasses import dataclass, field
from typing import Dict

HOST        = os.environ.get("GC_RELAY_HOST",  "0.0.0.0")
PORT        = int(os.environ.get("GC_RELAY_PORT", "8443"))
ADMIN_TOKEN = os.environ.get("GC_ADMIN_TOKEN",  "change-admin-token")

MAX_LINE_BYTES = 24 * 1024 * 1024


def b64_json(obj) -> str:
    raw = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    return base64.b64encode(raw).decode("ascii")


def now_id() -> str:
    return secrets.token_hex(8)


@dataclass
class AgentSession:
    device_id: str
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    peer: str
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)

    async def send_command(self, command_b64: str, timeout: float = 60.0) -> str:
        async with self.lock:
            request_id = now_id()
            self.writer.write(f"REQ {request_id} {command_b64}\n".encode("utf-8"))
            await self.writer.drain()

            line = await asyncio.wait_for(self.reader.readline(), timeout=timeout)
            if not line:
                raise ConnectionError("agent disconnected")
            if len(line) > MAX_LINE_BYTES:
                raise ValueError("agent response too large")

            text = line.decode("utf-8", errors="replace").strip()
            parts = text.split(" ", 2)
            if len(parts) != 3 or parts[0] != "RESP" or parts[1] != request_id:
                raise ValueError("invalid agent response")
            return parts[2]


agents: Dict[str, AgentSession] = {}


async def send_line(writer: asyncio.StreamWriter, text: str) -> None:
    writer.write((text + "\n").encode("utf-8"))
    await writer.drain()


async def handle_agent(first_line: str, reader: asyncio.StreamReader,
                       writer: asyncio.StreamWriter, peer: str) -> None:
    # Protocol: AGENT <device_id>   (no token required)
    parts = first_line.split(" ", 1)
    if len(parts) != 2 or not parts[1].strip():
        await send_line(writer, "ERR usage: AGENT <device_id>")
        writer.close()
        await writer.wait_closed()
        return

    device_id = parts[1].strip()

    # Disconnect any stale session with the same device_id.
    old = agents.get(device_id)
    if old:
        try:
            old.writer.close()
        except Exception:
            pass

    session = AgentSession(device_id=device_id, reader=reader, writer=writer, peer=peer)
    agents[device_id] = session
    await send_line(writer, "OK agent connected")
    print(f"[agent online]  {device_id} from {peer}")

    try:
        await writer.wait_closed()
    finally:
        if agents.get(device_id) is session:
            agents.pop(device_id, None)
            print(f"[agent offline] {device_id}")


async def handle_admin(first_line: str, reader: asyncio.StreamReader,
                       writer: asyncio.StreamWriter, peer: str) -> None:
    parts = first_line.split(" ", 1)
    if len(parts) != 2 or not secrets.compare_digest(parts[1].strip(), ADMIN_TOKEN):
        await send_line(writer, "ERR invalid admin token")
        writer.close()
        await writer.wait_closed()
        print(f"[admin denied]  from {peer}")
        return

    await send_line(writer, "OK admin connected")
    print(f"[admin online]  from {peer}")

    while not reader.at_eof():
        line = await reader.readline()
        if not line:
            break
        if len(line) > MAX_LINE_BYTES:
            await send_line(writer, "ERR request too large")
            continue

        text = line.decode("utf-8", errors="replace").strip()
        if not text:
            continue

        if text == "LIST":
            payload = {
                "agents": [
                    {"device_id": did, "peer": s.peer}
                    for did, s in sorted(agents.items())
                ]
            }
            await send_line(writer, "OK " + b64_json(payload))
            continue

        if text.startswith("CMD "):
            parts = text.split(" ", 2)
            if len(parts) != 3:
                await send_line(writer, "ERR usage: CMD <device_id> <base64-command>")
                continue

            _, device_id, command_b64 = parts
            session = agents.get(device_id)
            if not session:
                await send_line(writer, "ERR device not connected")
                continue

            try:
                response_b64 = await session.send_command(command_b64)
                await send_line(writer, "OK " + response_b64)
            except Exception as exc:
                try:
                    session.writer.close()
                except Exception:
                    pass
                if agents.get(device_id) is session:
                    agents.pop(device_id, None)
                await send_line(writer, "ERR " + str(exc))
            continue

        if text == "QUIT":
            await send_line(writer, "OK bye")
            break

        await send_line(writer, "ERR unknown relay command")

    writer.close()
    await writer.wait_closed()
    print(f"[admin offline] from {peer}")


async def handle_connection(reader: asyncio.StreamReader,
                            writer: asyncio.StreamWriter) -> None:
    peername = writer.get_extra_info("peername")
    peer = f"{peername[0]}:{peername[1]}" if peername else "unknown"

    try:
        line = await asyncio.wait_for(reader.readline(), timeout=10)
        if not line:
            return
        first = line.decode("utf-8", errors="replace").strip()
        if first.startswith("AGENT "):
            await handle_agent(first, reader, writer, peer)
        elif first.startswith("ADMIN "):
            await handle_admin(first, reader, writer, peer)
        else:
            await send_line(writer, "ERR first line must be AGENT or ADMIN")
            writer.close()
            await writer.wait_closed()
    except Exception as exc:
        print(f"[connection error] {peer}: {exc}")
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass


async def main() -> None:
    print("GrandControl relay starting")
    print(f"Listening on {HOST}:{PORT}")
    if ADMIN_TOKEN == "change-admin-token":
        print("WARNING: set GC_ADMIN_TOKEN before exposing this relay to the internet.")

    server = await asyncio.start_server(
        handle_connection,
        HOST,
        PORT,
        limit=MAX_LINE_BYTES,
    )
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nRelay stopped")
