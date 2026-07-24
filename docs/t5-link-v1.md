# T5-Link v1 frozen T5 contract

The canonical command IDs and golden wire bytes come from
`nightshift-opi/contracts/uart`. The normalized copy in this repository is
checked with:

```powershell
python tools/sync_golden_vectors.py --check
```

Frame format is `COBS(header + payload + CRC16) + 0x00`, little-endian,
protocol version 1, maximum payload 1024, CRC-16/CCITT-FALSE.

## Compatibility decisions

The live Orange Pi implementation and golden vectors take precedence over the
older joint design document:

- `HELLO.capabilities` is `u16`.
- `MODE_SET` is `<IBIQ>`: `revision:u32`, `mode:u8`, `reason:u32`,
  `changed_at_ms:u64`. Payload length is 17, with `changed_at_ms` at offset 9.
- `WORK_STATE_SET` has no revision on wire. Its fixed prefix is
  `<BHHIIII>` followed by a string. The unused Python `revision` argument does
  not change the encoded bytes.
- A heartbeat response payload is status `u16` plus three `u32` fields: 14
  bytes total.
- A UI action fixed prefix is `<HBIi>` (11 bytes), followed by a string.

## Implemented payloads

Strings are `u16 byte_length + UTF-8`, without a terminating NUL.

| Command | Payload |
|---|---|
| `HELLO` | `u8 role, u8 major, u8 minor, u32 boot_id, u16 max_payload, u16 capabilities, string version` |
| `HEARTBEAT` | `u32 uptime_ms, u32 state_revision` |
| `GET_INFO` response data | `u8 proto_major, u8 proto_minor, u16 max_payload, u32 capabilities, u16 width, u16 height, u8 color_bits, u8 max_tasks, string firmware, string board` |
| `TIME_SYNC` | `u64 unix_time_ms, i16 utc_offset_minutes` |
| `STATE_SYNC_BEGIN` | `u32 revision, u8 reason` |
| `STATE_SYNC_END` | `u32 revision, u32 snapshot_crc32` |
| `MODE_SET` | `u32 revision, u8 mode, u32 reason, u64 changed_at_ms` |
| `ATTENTION_SET` | `u32 revision, u32 flags, u16 count, string message` |
| `WORK_STATE_SET` | `u8 state, u16 progress, u16 reserved, u32 token_in, u32 token_out, u32 elapsed_s, u32 task_id, string title` |
| `DASHBOARD_SET` | `u32 revision, six u16 counters` |
| `NOTICE_SHOW` | `u32 revision, u32 notice_id, u8 severity, u8 flags, u64 expires_at_ms, string title, string body` |
| `TASK_LIST_BEGIN` | `u32 revision, u8 list_type, u16 item_count` |
| `TASK_ITEM` | `u32 revision, u32 task_id, u8 quadrant, u8 state, u8 flags, string title, string source` |
| `TASK_LIST_END` | `u32 revision, u32 list_crc32` |
| `UI_ACTION` | `u16 action, u8 object_type, u32 object_id, i32 value, string text` |
| `PAGE_EVENT` | `u8 page_id, u8 event, u32 object_id` |
| `LED_OVERRIDE` | `u8 active, u8 mode, u16 period_ms` |
| `BACKLIGHT_SET` | `u8 percent` |

`NOTICE_SHOW`, `PAGE_EVENT`, `LED_OVERRIDE`, and `BACKLIGHT_SET` had command
IDs but no field definitions in the Orange Pi YAML. The layouts above are
minor-compatible extensions and must be copied into the Orange Pi contract
before that host begins sending them.

## Atomic synchronization

During a full sync, the T5 stages all changes and retains the committed screen.
`MODE_SET`, `ATTENTION_SET`, and `WORK_STATE_SET` are required. Task-list
transactions must be complete. `STATE_SYNC_END` commits once and posts one UI
refresh. A nonzero snapshot CRC is CRC32/IEEE over each accepted command ID
(u16 LE) followed by its raw payload, in arrival order. Zero means not supplied,
matching the current Orange Pi.

Task list CRC, when nonzero, is CRC32/IEEE over the raw `TASK_ITEM` payloads in
arrival order. Incomplete or mismatched transactions are discarded.

## Events

`UI_ACTION` is sent as `EVENT | ACK_REQ`; retrying uses the same sequence and
the T5 disables side-effect controls while waiting. The current Orange Pi
gateway must echo that sequence and command in a `RESPONSE` frame.

Object types are: 0 none, 1 task, 2 notice, 3 executor, 4 panel. Action 13 is
the optional `REQUEST_RESYNC` extension; actions 1–12 retain the Orange Pi
contract values.
