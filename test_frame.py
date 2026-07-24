"""Inject a T5-Link v1 ATTENTION_SET frame into COM7 and print the reply.

Frame: Magic(2) Ver(1) Flags(1) Seq(2LE) Cmd(2LE) Len(2LE) Payload CRC16(2LE)
Wire : COBS(raw) + 0x00
"""
import serial
import struct
import time


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def cobs_encode(src: bytes) -> bytes:
    out = bytearray()
    code_idx = 0
    code = 1
    out.append(0)  # reserve first code slot
    for b in src:
        if b == 0x00:
            out[code_idx] = code
            code = 1
            code_idx = len(out)
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_idx] = 0xFF
                code_idx = len(out)
                out.append(0)
                code = 1
    out[code_idx] = code
    return bytes(out)


def build_frame(flags: int, seq: int, cmd: int, payload: bytes) -> bytes:
    raw = bytes([0x54, 0x35, 0x01, flags])
    raw += struct.pack('<HHH', seq, cmd, len(payload))
    raw += payload
    raw += struct.pack('<H', crc16_ccitt_false(raw))
    return cobs_encode(raw) + b'\x00'


def main():
    # ATTENTION_SET (0x1002), ACK_REQ, attention = NEED_CONFIRM (u32LE)
    frame = build_frame(flags=0x01, seq=42, cmd=0x1002,
                        payload=struct.pack('<I', 0x00000001))
    print('TX frame:', frame.hex(' '))

    s = serial.Serial('COM7', 460800, timeout=1)
    s.reset_input_buffer()
    s.write(frame)
    s.flush()
    time.sleep(4)
    data = s.read(s.in_waiting)
    s.close()

    print(f'--- {len(data)} bytes received ---')
    print(data.decode('utf-8', 'replace'))
    print('--- raw hex ---')
    print(data.hex(' '))


if __name__ == '__main__':
    main()
