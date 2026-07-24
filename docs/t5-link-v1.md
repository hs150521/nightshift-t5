# T5-Link v1 frozen shared contract

The source of truth is the independently frozen schema in
`contracts/uart/commands.yaml`, not either endpoint's current implementation.
The T5 and Orange Pi repositories carry byte-identical schema and generated
golden-vector artifacts. Check both copies with:

```powershell
python tools/regenerate_golden_vectors.py
python tools/sync_golden_vectors.py --source-root ../nightshift-opi
```

Frame format is `COBS(header + payload + CRC16) + 0x00`, little-endian,
protocol version 1, maximum payload 1024, CRC-16/CCITT-FALSE.

## Frozen layout decisions

- `HELLO.capabilities` is `u16`.
- `MODE_SET` is `<IBBQ>`: `revision:u32`, `mode:u8`, `reason:u8`,
  `changed_at_ms:u64`. Payload length is exactly 14.
- `WORK_STATE_SET` is `<IBHHIIII>` followed by a string. Its fixed prefix is
  25 bytes and its minimum payload length is 27.
- A heartbeat response frame payload is `<HIII>`: `status:u16`,
  `t5_uptime_ms:u32`, `applied_revision:u32`, `error_flags:u32`. It is exactly
  14 bytes.
- A UI action fixed prefix is `<HBIi>` (11 bytes), followed by a string.

## Implemented payloads

Strings are `u16 byte_length + UTF-8`, without a terminating NUL.

| Command | Payload |
|---|---|
| `HELLO` | `u8 role, u8 major, u8 minor, u32 boot_id, u16 max_payload, u16 capabilities, string version` |
| `HEARTBEAT` request | `u32 uptime_ms, u32 state_revision` |
| `HEARTBEAT` response frame | `u16 status, u32 t5_uptime_ms, u32 applied_revision, u32 error_flags` |
| `GET_INFO` response data | `u8 proto_major, u8 proto_minor, u16 max_payload, u32 capabilities, u16 width, u16 height, u8 color_bits, u8 max_tasks, string firmware, string board` |
| `TIME_SYNC` | `u64 unix_time_ms, i16 utc_offset_minutes` |
| `STATE_SYNC_BEGIN` | `u32 revision, u8 reason` |
| `STATE_SYNC_END` | `u32 revision, u32 snapshot_crc32` |
| `MODE_SET` | `u32 revision, u8 mode, u8 reason, u64 changed_at_ms` |
| `ATTENTION_SET` | `u32 revision, u32 flags, u16 count, string message` |
| `WORK_STATE_SET` | `u32 revision, u8 state, u16 progress, u16 reserved, u32 token_in, u32 token_out, u32 elapsed_s, u32 task_id, string title` |
| `DASHBOARD_SET` | `u32 revision, six u16 counters` |
| `NOTICE_SHOW` | `u32 revision, u32 notice_id, u8 severity, u8 flags, u64 expires_at_ms, string title, string body` |
| `TASK_LIST_BEGIN` | `u32 revision, u8 list_type, u16 item_count` |
| `TASK_ITEM` | `u32 revision, u32 task_id, u8 quadrant, u8 state, u8 flags, string title, string source` |
| `TASK_LIST_END` | `u32 revision, u32 list_crc32` |
| `UI_ACTION` | `u16 action, u8 object_type, u32 object_id, i32 value, string text` |
| `PAGE_EVENT` | `u8 page_id, u8 event, u32 object_id` |
| `LED_OVERRIDE` | `u8 active, u8 mode, u16 period_ms` |
| `BACKLIGHT_SET` | `u8 percent` |

`NOTICE_SHOW`, `PAGE_EVENT`, `LED_OVERRIDE`, and `BACKLIGHT_SET` are formal
members of the frozen schema. Their layouts must change only through an atomic
contract release covering both endpoints and regenerated vectors.

## Atomic synchronization

During a full sync, the T5 stages all changes and retains the committed screen.
`MODE_SET`, `ATTENTION_SET`, and `WORK_STATE_SET` are required. Task-list
transactions must be complete. `STATE_SYNC_END` commits once and posts one UI
refresh. `STATE_SYNC_BEGIN` rejects a target below the committed revision,
while an explicit same-revision resync is allowed. Every revisioned snapshot
component, including `WORK_STATE_SET`, must equal the target revision.
A nonzero snapshot CRC is CRC32/IEEE over each accepted command ID
(u16 LE) followed by its raw payload, in arrival order. Zero means not supplied,
matching the current Orange Pi.

Task list CRC, when nonzero, is CRC32/IEEE over the raw `TASK_ITEM` payloads in
arrival order. Incomplete or mismatched transactions are discarded.

## Events

`UI_ACTION` is sent as `EVENT | ACK_REQ`. On ACK timeout, the T5 retransmits
the identical sequence, command, and payload up to two times. It clears the
pending action on a matching response or after the retry limit. The Orange Pi
must echo the event sequence and command in its `RESPONSE` frame.

The offline watchdog is driven only by valid `HEARTBEAT` requests. A valid
`HELLO` starts one initial six-second grace window; other commands do not move
the deadline. A valid heartbeat recovers the link and permits a full resync.

Object types are: 0 none, 1 task, 2 notice, 3 executor, 4 panel. Action 13 is
the optional `REQUEST_RESYNC` extension; actions 1–12 retain the Orange Pi
contract values.
