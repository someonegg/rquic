# RQUIC Wireshark plugin

Copy `rquic.lua` to the **Personal Lua Plugins** directory reported by
`tshark -G folders`, then restart Wireshark or reload Lua plugins.

The plugin detects UDP payloads by validating packet and frame boundaries.
It does not require TLS keys. For truncated packets or unknown extensions,
use **Decode As → RQUIC**, or configure UDP ports under **Preferences →
Protocols → RQUIC**. Disable heuristic detection there if it conflicts with
another UDP protocol.

Short headers do not encode CID length. The plugin always uses the configured
**Short-header DCID length** (8 bytes by default), which must match the receiving
engine's CID length. Long headers use their explicit CID length fields.
Packet numbers are displayed as transmitted, without
reconstructing the full number. ACK delay is the encoded value, before applying
the negotiated exponent. STREAM data is shown per frame, without stream
reassembly or application protocol decoding.

Supported frames: PADDING, PING, ACK/ACK_ECN, HANDSHAKE (ALPN, transport
parameter TLVs, protocol extension), STREAM, RESET_STREAM, STOP_SENDING,
flow control, PATH_CHALLENGE/PATH_RESPONSE, and CONNECTION_CLOSE.
Unknown frames stop parsing the current packet and show a warning. Invalid
lengths and truncated fields show a malformed diagnostic.

Useful filters: `rquic`, `rquic.stream.id == 88`, `rquic.frame.type == 6`,
`rquic.ack.largest`, `rquic.malformed`.

For temporary loading without installation:

```sh
tshark -X lua_script:tools/wireshark/rquic.lua -r capture.pcap -V
```

Validated with Wireshark 4.6: the supplied 135-packet capture and a complete
6-packet exchange captured through a UDP relay using the project's demo
client/server. Coalesced packets, truncation, unknown frames and unrelated
UDP payloads were also checked.
