#!/usr/bin/env python3
import socket
import struct
import wave
import os
import io
import sys
import threading
import argparse
import time
import errno

# --- CONFIG ---
HOST = "0.0.0.0"  # Listen on all interfaces
PORT = 5000       # Default port
MODEL_PATH = "../vosk-model-en-us-0.22"  # Change to your downloaded model
REPLY_WAV = "./reply.wav"  # Stock WAV to send back

# --- Echo or Reply mode ---
ECHO_MODE = True  # Default: Echo back the received audio

# --- Optional: TTS via pyttsx3 ---
use_tts = False
try:
    import pyttsx3
    tts = pyttsx3.init()
    use_tts = True
except ImportError:
    print("[INFO] pyttsx3 not available. Speech output disabled.")

# --- Optional: STT via Vosk (offline) ---
use_vosk = False
try:
    from vosk import Model, KaldiRecognizer
    if os.path.isdir(MODEL_PATH):
        vosk_model = Model(MODEL_PATH)
        use_vosk = True
        print(f"[INFO] Vosk model loaded from: {MODEL_PATH}")
    else:
        print(f"[WARN] Vosk model folder not found: {MODEL_PATH}")
except ImportError:
    print("[INFO] Vosk not available. Speech recognition disabled.")
except Exception as e:
    print(f"[WARN] Error loading Vosk: {e}")

# Protocol configuration
PROTOCOL_VERSION = 0x01
PACKET_HEADER_SIZE = 8
PACKET_MAX_SIZE = 4096
PACKET_TYPE_HANDSHAKE = 0x01
PACKET_TYPE_DATA = 0x02
PACKET_TYPE_DATA_END = 0x03
PACKET_TYPE_ACK = 0x04
PACKET_TYPE_NACK = 0x05
PACKET_TYPE_RETRY = 0x06

# For tracking sequence numbers per client
client_sequences = {}

def read_exact(conn, n):
    """Read exactly n bytes from the connection"""
    buf = b""
    while len(buf) < n:
        try:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                return None  # Connection closed
            buf += chunk
        except socket.timeout:
            print("[WARN] Socket read timeout")
            return None
        except ConnectionResetError:
            print("[WARN] Connection reset by peer")
            return None
        except Exception as e:
            print(f"[ERROR] Socket read error: {e}")
            return None
    return buf

def send_exact(conn, data):
    """Send data with error handling"""
    try:
        conn.sendall(data)
        return True
    except socket.timeout:
        print("[WARN] Socket write timeout")
        return False
    except BrokenPipeError:
        print("[WARN] Broken pipe - client disconnected")
        return False
    except ConnectionResetError:
        print("[WARN] Connection reset by peer")
        return False
    except Exception as e:
        print(f"[ERROR] Socket write error: {e}")
        return False

def calculate_checksum(data):
    """Calculate Fletcher-16 checksum for data verification"""
    if isinstance(data, memoryview):
        data = data.tobytes()
    
    sum1 = 0
    sum2 = 0
    
    for byte in data:
        sum1 = (sum1 + byte) % 255
        sum2 = (sum2 + sum1) % 255
    
    return (sum2 << 8) | sum1

def build_packet_header(data_length, packet_type, checksum):
    """Create a packet header"""
    # 4 bytes: length (big endian)
    # 1 byte: protocol version
    # 1 byte: packet type
    # 2 bytes: checksum (big endian)
    return struct.pack(">IBBH", data_length, PROTOCOL_VERSION, packet_type, checksum)

def parse_packet_header(header):
    """Parse a packet header"""
    if len(header) != PACKET_HEADER_SIZE:
        return None
    
    data_length, version, packet_type, checksum = struct.unpack(">IBBH", header)
    
    # Verify protocol version
    if version != PROTOCOL_VERSION:
        return None
    
    return {
        "length": data_length,
        "version": version,
        "type": packet_type,
        "checksum": checksum
    }

def send_ack(conn):
    """Send acknowledgment packet with flush to ensure immediate delivery"""
    header = build_packet_header(0, PACKET_TYPE_ACK, 0)
    try:
        conn.sendall(header)
        # Add debug print to track sending
        print(f"[DEBUG] Sending ACK header: {' '.join(['0x{:02X}'.format(b) for b in header])}")
        return True
    except Exception as e:
        print(f"[ERROR] Failed to send ACK: {e}")
        return False

def send_nack(conn):
    """Send negative acknowledgment packet"""
    header = build_packet_header(0, PACKET_TYPE_NACK, 0)
    try:
        conn.sendall(header)
        return True
    except:
        return False

def handle_handshake(conn, addr):
    """Handle protocol handshake with improved debugging and reliability"""
    client_ip = f"{addr[0]}:{addr[1]}"

    try:
        # Read handshake data length from header
        print(f"[DEBUG] {client_ip}: Waiting for handshake header ({PACKET_HEADER_SIZE} bytes)")
        header_data = read_exact(conn, PACKET_HEADER_SIZE)
        if not header_data:
            print(f"[-] {client_ip}: Failed to read handshake header")
            return False

        print(f"[DEBUG] {client_ip}: Received handshake header: {' '.join(['0x{:02X}'.format(b) for b in header_data])}")

        header_info = parse_packet_header(header_data)
        if not header_info or header_info["type"] != PACKET_TYPE_HANDSHAKE:
            print(f"[-] {client_ip}: Invalid handshake header, type={header_info.get('type') if header_info else 'None'}")
            return False

        # Read handshake data
        print(f"[DEBUG] {client_ip}: Waiting for handshake data ({header_info['length']} bytes)")
        handshake_data = read_exact(conn, header_info["length"])
        if not handshake_data:
            print(f"[-] {client_ip}: Failed to read handshake data")
            return False

        print(f"[DEBUG] {client_ip}: Received handshake data: {' '.join(['0x{:02X}'.format(b) for b in handshake_data])}")

        # Verify handshake data checksum
        actual_checksum = calculate_checksum(handshake_data)
        if actual_checksum != header_info["checksum"]:
            print(f"[-] {client_ip}: Handshake checksum mismatch: expected 0x{header_info['checksum']:04X}, got 0x{actual_checksum:04X}")
            return False

        # Check for magic bytes (A5 5A B2 2B)
        expected_magic = b'\xA5\x5A\xB2\x2B'
        if handshake_data != expected_magic:
            print(f"[-] {client_ip}: Invalid handshake magic bytes: {' '.join(['0x{:02X}'.format(b) for b in handshake_data])}")
            return False

        # Initialize sequence tracking for this client
        client_sequences[client_ip] = {
            "send": 0,
            "receive": 0
        }

        # Add a small delay before sending ACK to ensure client is ready to receive
        time.sleep(0.05)

        # Send acknowledgment
        print(f"[DEBUG] {client_ip}: Sending handshake ACK")
        if not send_ack(conn):
            print(f"[-] {client_ip}: Failed to send handshake ACK")
            return False

        # Add a small delay after sending ACK
        time.sleep(0.05)

        print(f"[+] {client_ip}: Handshake successful")
        return True

    except Exception as e:
        print(f"[-] {client_ip}: Handshake error: {e}")
        return False

# === Synchronized Chunk Size Configuration for Server ===

# Add/update these constants at the top of your echo_test_server.py file
# Make sure they match exactly with the client side settings

# Protocol configuration
PROTOCOL_VERSION = 0x01
PACKET_HEADER_SIZE = 8
PACKET_MAX_SIZE = 8192
PACKET_TYPE_HANDSHAKE = 0x01
PACKET_TYPE_DATA = 0x02
PACKET_TYPE_DATA_END = 0x03
PACKET_TYPE_ACK = 0x04
PACKET_TYPE_NACK = 0x05
PACKET_TYPE_RETRY = 0x06

def receive_data_with_verification(conn, addr, max_size=10*1024*1024):
    """
    Receive data with verification and acknowledgment
    Returns received data or None on failure
    """
    client_ip = f"{addr[0]}:{addr[1]}"

    # Initialize or get sequence counter for this client
    if client_ip not in client_sequences:
        client_sequences[client_ip] = {"send": 0, "receive": 0}

    receive_sequence = client_sequences[client_ip]["receive"]
    received_data = bytearray()

    print(f"[DEBUG] {client_ip}: Ready to receive data (max packet size: {PACKET_MAX_SIZE} bytes)")

    while True:
        # Read packet header
        header_data = read_exact(conn, PACKET_HEADER_SIZE)
        if not header_data:
            print(f"[-] {client_ip}: Failed to read packet header")
            return None

        header_info = parse_packet_header(header_data)
        if not header_info:
            print(f"[-] {client_ip}: Invalid packet header")
            send_nack(conn)
            return None

        # Validate data length
        data_length = header_info["length"]

        # Debug output for large packets
        if data_length > 1024:  # Only log when packets are larger than original size
            print(f"[DEBUG] {client_ip}: Receiving packet of size {data_length} bytes (max: {PACKET_MAX_SIZE})")

        if data_length > PACKET_MAX_SIZE:
            print(f"[-] {client_ip}: Packet too large ({data_length} bytes, max: {PACKET_MAX_SIZE})")
            send_nack(conn)
            return None

        if len(received_data) + data_length > max_size:
            print(f"[-] {client_ip}: Total data exceeds maximum ({len(received_data) + data_length} > {max_size})")
            send_nack(conn)
            return None

        # Read sequence number (first 2 bytes)
        seq_bytes = read_exact(conn, 2)
        if not seq_bytes:
            print(f"[-] {client_ip}: Failed to read sequence number")
            send_nack(conn)
            return None

        packet_sequence = struct.unpack(">H", seq_bytes)[0]

        # Verify sequence number
        if packet_sequence != receive_sequence:
            print(f"[-] {client_ip}: Sequence mismatch: expected {receive_sequence}, got {packet_sequence}")
            send_nack(conn)
            continue

        # Read chunk data
        chunk_data = read_exact(conn, data_length)
        if not chunk_data:
            print(f"[-] {client_ip}: Failed to read chunk data")
            send_nack(conn)
            return None

        # Verify checksum
        actual_checksum = calculate_checksum(chunk_data)
        if actual_checksum != header_info["checksum"]:
            print(f"[-] {client_ip}: Checksum mismatch: expected {header_info['checksum']}, got {actual_checksum}")
            send_nack(conn)
            continue

        # Send acknowledgment
        if not send_ack(conn):
            print(f"[-] {client_ip}: Failed to send ACK")
            return None

        # Append data
        received_data.extend(chunk_data)
        receive_sequence += 1
        client_sequences[client_ip]["receive"] = receive_sequence

        # Check if this was the last packet
        if header_info["type"] == PACKET_TYPE_DATA_END:
            break

    return received_data

def send_data_with_retries(conn, addr, data, max_retries=5):
    """
    Send data with improved retry logic and better client synchronization
    Returns True on success, False on failure
    """
    client_ip = f"{addr[0]}:{addr[1]}"

    # Initialize or get sequence counter for this client
    if client_ip not in client_sequences:
        client_sequences[client_ip] = {"send": 0, "receive": 0}

    send_sequence = client_sequences[client_ip]["send"]
    total_sent = 0
    # Use PACKET_MAX_SIZE constant to ensure consistency
    chunk_size = PACKET_MAX_SIZE
    data_length = len(data)

    print(f"[+] {client_ip}: Preparing to send response ({len(data)} bytes, chunk size: {chunk_size})")

    # Wait a moment before starting to send
    time.sleep(0.2)

    # Loop through data in chunks
    while total_sent < data_length:
        remaining = data_length - total_sent
        current_chunk_size = min(remaining, chunk_size)
        is_last_chunk = (total_sent + current_chunk_size >= data_length)

        # Determine packet type
        packet_type = PACKET_TYPE_DATA_END if is_last_chunk else PACKET_TYPE_DATA

        # Get current chunk
        chunk = data[total_sent:total_sent+current_chunk_size]

        # Calculate checksum
        checksum = calculate_checksum(chunk)

        # Create header
        header = build_packet_header(current_chunk_size, packet_type, checksum)

        # Add sequence number
        sequence_bytes = struct.pack(">H", send_sequence)

        # Retry logic with progressive backoff
        chunk_sent = False

        for retry in range(max_retries):
            if retry > 0:
                retry_delay = min(0.1 * (2 ** retry), 2.0)  # Exponential backoff
                print(f"[+] {client_ip}: Retry {retry}/{max_retries} after {retry_delay:.2f}s")
                time.sleep(retry_delay)

            try:
                # Send header
                conn.sendall(header)

                # Send sequence number
                conn.sendall(sequence_bytes)

                # Send chunk data
                conn.sendall(chunk)

                # Wait for acknowledgment with timeout
                conn.settimeout(2.0)  # 2 second timeout for ACK
                ack_header = read_exact(conn, PACKET_HEADER_SIZE)
                conn.settimeout(30.0)  # Reset to normal timeout

                if not ack_header:
                    print(f"[-] {client_ip}: No ACK received")
                    continue

                ack_info = parse_packet_header(ack_header)
                if not ack_info:
                    print(f"[-] {client_ip}: Invalid ACK header")
                    continue

                if ack_info["type"] == PACKET_TYPE_ACK:
                    chunk_sent = True
                    break
                elif ack_info["type"] == PACKET_TYPE_NACK:
                    print(f"[-] {client_ip}: Received NACK")
                    continue

            except Exception as e:
                print(f"[-] {client_ip}: Send error: {e}")
                continue

        if not chunk_sent:
            print(f"[-] {client_ip}: Failed to send chunk after {max_retries} retries")
            return False

        # Update counters
        total_sent += current_chunk_size
        send_sequence += 1
        client_sequences[client_ip]["send"] = send_sequence

        # Progress report for first/last chunk and every 10%
        if total_sent == current_chunk_size or is_last_chunk or total_sent % (data_length // 10) < chunk_size:
            percent = int((total_sent * 100) / data_length)
            print(f"[+] {client_ip}: Sent {total_sent}/{data_length} bytes ({percent}%)")

    return True

def create_test_wav():
    """Create a test WAV file with a beep tone"""
    try:
        import numpy as np

        # Parameters
        duration = 1.0  # seconds
        sample_rate = 16000  # Hz
        frequency = 440.0  # Hz (A4 note)

        # Generate samples
        t = np.linspace(0, duration, int(sample_rate * duration), False)
        samples = (np.sin(2 * np.pi * frequency * t) * 32767).astype(np.int16)

        # Create WAV file
        with wave.open("reply.wav", "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(sample_rate)
            wf.writeframes(samples.tobytes())

        print("Created test reply.wav file")
    except ImportError:
        print("[INFO] NumPy not available. Cannot create test WAV file.")

def speak_async(text: str):
    """Speak text asynchronously using pyttsx3"""
    if not use_tts:
        print(f"[TTS] Would say: {text}")
        return

    def run():
        try:
            tts.say(text)
            tts.runAndWait()
        except Exception as e:
            print(f"[TTS error] {e}")

    threading.Thread(target=run, daemon=True).start()

def transcribe_wav_bytes(wav_bytes: bytes) -> str:
    """
    Transcribe WAV bytes using Vosk speech recognition.
    Expects a standard PCM WAV (16-bit mono preferred).
    Returns recognized text or "".
    """
    if not use_vosk:
        return ""

    try:
        with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
            sr = wf.getframerate()
            ch = wf.getnchannels()
            sw = wf.getsampwidth()  # 2 for 16-bit

            print(f"[INFO] WAV format: {sr}Hz, {sw*8}-bit, {ch} channels")

            if sw != 2:
                print(f"[STT] Non-16-bit audio (sampwidth={sw}) -> might be unsupported")
            if ch != 1:
                print(f"[STT] Non-mono audio (channels={ch}) -> downmix recommended")

            rec = KaldiRecognizer(vosk_model, sr)
            rec.SetWords(True)

            text_parts = []
            while True:
                data = wf.readframes(4096)
                if len(data) == 0:
                    break
                if rec.AcceptWaveform(data):
                    text_parts.append(rec.Result())
            text_parts.append(rec.FinalResult())

        # Extract plain text from Vosk JSON results
        import json, re
        combined = " ".join(text_parts)
        try:
            # The last result is the final JSON; others are intermediate JSON strings.
            # We'll scan for all "text" fields in order.
            texts = re.findall(r'"text"\s*:\s*"([^"]*)"', combined)
            return " ".join(t for t in texts if t).strip()
        except Exception as e:
            print(f"[STT] Error parsing results: {e}")
            return ""
    except Exception as e:
        print(f"[STT error] {e}")
        return ""

def verify_wav_format(wav_bytes):
    """Verify that the bytes represent a valid WAV file"""
    if len(wav_bytes) < 44:  # Minimum WAV header size
        return False

    try:
        # Check RIFF header
        if wav_bytes[0:4] != b'RIFF':
            return False
        # Check WAVE format
        if wav_bytes[8:12] != b'WAVE':
            return False
        # Check fmt chunk
        if wav_bytes[12:16] != b'fmt ':
            return False
        # Check data chunk identifier
        if wav_bytes[36:40] != b'data':
            return False
        return True
    except Exception:
        return False

def ensure_valid_wav_format(audio_data):
    """
    Ensures the audio data has a valid WAV format
    Returns corrected WAV data
    """
    # First, check if the data already appears to be a valid WAV
    if len(audio_data) >= 44 and audio_data[0:4] == b'RIFF' and audio_data[8:12] == b'WAVE':
        print("[DEBUG] Audio data already has valid WAV header")
        return audio_data

    # If it's not a valid WAV, we need to create one
    print("[DEBUG] Creating valid WAV format from audio data")

    # Determine if this is raw PCM data or a corrupted WAV
    is_likely_pcm = True
    if len(audio_data) >= 12:
        # Check for partial WAV header
        if audio_data[0:4] == b'RIFF' or audio_data[8:12] == b'WAVE':
            is_likely_pcm = False
            print("[DEBUG] Data appears to be a corrupted WAV file")
        else:
            print("[DEBUG] Data appears to be raw PCM")

    if is_likely_pcm:
        # Assume 16-bit mono PCM at 16kHz
        return create_wav_from_pcm(audio_data, sample_rate=16000, channels=1, sample_width=2)
    else:
        # Try to repair corrupted WAV
        return repair_wav_file(audio_data)

def create_wav_from_pcm(pcm_data, sample_rate=16000, channels=1, sample_width=2):
    """
    Creates a valid WAV file from PCM audio data
    """
    import io
    import wave

    # Create a WAV file in memory
    wav_buffer = io.BytesIO()
    with wave.open(wav_buffer, 'wb') as wav_file:
        wav_file.setnchannels(channels)
        wav_file.setsampwidth(sample_width)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_data)

    # Get the WAV data
    wav_data = wav_buffer.getvalue()

    print(f"[DEBUG] Created WAV from PCM: {len(wav_data)} bytes, {sample_rate}Hz, {channels} channels, {sample_width*8}-bit")
    return wav_data

def repair_wav_file(corrupted_wav):
    """
    Attempts to repair a corrupted WAV file
    """
    # If we have enough bytes to work with
    if len(corrupted_wav) < 44:
        # Not enough data for a WAV header, treat as PCM
        return create_wav_from_pcm(corrupted_wav)
    
    # Create a new buffer for the repaired WAV
    repaired_wav = bytearray(len(corrupted_wav))
    
    # Copy the data
    repaired_wav[:] = corrupted_wav
    
    # Fix the header
    repaired_wav[0:4] = b'RIFF'
    repaired_wav[8:12] = b'WAVE'
    repaired_wav[12:16] = b'fmt '
    
    # Fix chunk sizes
    import struct
    
    # Calculate chunk sizes
    data_size = len(corrupted_wav) - 44  # Assuming standard 44-byte header
    
    # Set data chunk size
    repaired_wav[40:44] = struct.pack('<I', data_size)
    
    # Set overall chunk size (file size - 8)
    repaired_wav[4:8] = struct.pack('<I', len(corrupted_wav) - 8)
    
    # Set fmt chunk size (16 for PCM)
    repaired_wav[16:20] = struct.pack('<I', 16)
    
    # Set format tag (1 for PCM)
    repaired_wav[20:22] = struct.pack('<H', 1)
    
    # Set number of channels (1 for mono)
    repaired_wav[22:24] = struct.pack('<H', 1)
    
    # Set sample rate (16000 Hz)
    repaired_wav[24:28] = struct.pack('<I', 16000)
    
    # Set byte rate (sample_rate * num_channels * bytes_per_sample)
    repaired_wav[28:32] = struct.pack('<I', 16000 * 1 * 2)
    
    # Set block align (num_channels * bytes_per_sample)
    repaired_wav[32:34] = struct.pack('<H', 1 * 2)
    
    # Set bits per sample (16)
    repaired_wav[34:36] = struct.pack('<H', 16)
    
    # Set data marker
    repaired_wav[36:40] = b'data'
    
    print(f"[DEBUG] Repaired WAV header")
    
    # Log header for debugging
    header_bytes = ' '.join([f'{b:02X}' for b in repaired_wav[:44]])
    print(f"[DEBUG] Repaired header: {header_bytes}")
    
    return bytes(repaired_wav)

def process_audio(wav_bytes):
    """
    Process audio and ensure it has a valid WAV format
    """
    # First, validate the WAV format
    is_valid_wav = False
    if len(wav_bytes) >= 44:
        if wav_bytes[0:4] == b'RIFF' and wav_bytes[8:12] == b'WAVE':
            is_valid_wav = True

    if not is_valid_wav:
        print("[WARN] Invalid WAV format detected, attempting to repair")
        wav_bytes = ensure_valid_wav_format(wav_bytes)

    # Now perform any audio processing you want
    # For now, we'll just return the (potentially fixed) WAV data
    return wav_bytes

def make_silent_wav(sr=16000, ms=250):
    """Return bytes of a 16-bit mono silent WAV of given duration."""
    import struct as st

    # Calculate number of samples
    n = int(sr * ms / 1000)
    # Create silent PCM data (all zeros)
    pcm = b"\x00\x00" * n

    # Build WAV header
    subchunk2_size = len(pcm)
    chunk_size = 36 + subchunk2_size

    # RIFF header
    header = b"RIFF" + st.pack("<I", chunk_size) + b"WAVE"
    # fmt chunk
    header += b"fmt " + st.pack("<IHHIIHH",
                               16,       # Subchunk1Size (16 for PCM)
                               1,        # AudioFormat (1 for PCM)
                               1,        # NumChannels (1 for mono)
                               sr,       # SampleRate
                               sr*2,     # ByteRate (SampleRate * NumChannels * BitsPerSample/8)
                               2,        # BlockAlign (NumChannels * BitsPerSample/8)
                               16)       # BitsPerSample
    # data chunk
    header += b"data" + st.pack("<I", subchunk2_size)

    return header + pcm

def handle_connection(conn, addr):
    """Original connection handler (kept for reference)"""
    client_ip = f"{addr[0]}:{addr[1]}"
    print(f"[+] Connected: {client_ip}")

    try:
        # 1) Read incoming length (4-byte big-endian)
        raw = read_exact(conn, 4)
        if not raw:
            print(f"[-] {client_ip}: Connection closed before length received")
            return

        (length,) = struct.unpack(">I", raw)
        print(f"[INFO] {client_ip}: Expecting {length} bytes of audio data")

        if length <= 0 or length > 10 * 1024 * 1024:  # Sanity check: max 10MB
            print(f"[-] {client_ip}: Invalid length: {length}")
            return

        # 2) Read WAV bytes
        start_time = time.time()
        data = read_exact(conn, length)
        if data is None:
            print(f"[-] {client_ip}: Connection closed during data transfer")
            return

        transfer_time = time.time() - start_time
        transfer_rate = length / transfer_time / 1024 if transfer_time > 0 else 0
        print(f"[INFO] {client_ip}: Received {len(data)} bytes in {transfer_time:.2f}s ({transfer_rate:.1f} KB/s)")

        # 3) Save input WAV for debugging
        os.makedirs("inbox", exist_ok=True)
        timestamp = time.strftime("%Y%m%d-%H%M%S")
        input_filename = f"inbox/{timestamp}_{addr[0]}.wav"

        with open(input_filename, "wb") as f:
            f.write(data)
        print(f"[INFO] Saved {input_filename} ({len(data)} bytes)")

        # 4) Verify it's a valid WAV
        if not verify_wav_format(data):
            print(f"[WARN] {client_ip}: Received data is not a valid WAV file")
            # We'll still try to process it

        # 5) Transcribe using Vosk if available
        text = transcribe_wav_bytes(data)
        if text:
            print(f"[STT] {client_ip} said: \"{text}\"")
            if use_tts:
                speak_async(text)  # Speak it out loud on the server
        else:
            print(f"[STT] {client_ip}: No text recognized or STT disabled")
            if use_tts:
                speak_async("Audio received")

        # 6) Prepare reply
        if ECHO_MODE:
            # Echo mode: send back the same audio that was received
            reply_bytes = process_audio(data)
            mode_str = "ECHO"
        else:
            # Reply mode: send a stock WAV file
            if os.path.isfile(REPLY_WAV):
                with open(REPLY_WAV, "rb") as f:
                    reply_bytes = f.read()
                mode_str = "REPLY"
            else:
                print(f"[-] Reply WAV missing: {REPLY_WAV}")
                # Send a tiny silent WAV as fallback
                reply_bytes = make_silent_wav(sr=16000, ms=250)
                mode_str = "SILENT"

        # 7) Send length header - handle send errors
        length_header = struct.pack(">I", len(reply_bytes))
        if not send_exact(conn, length_header):
            print(f"[-] {client_ip}: Failed to send length header")
            return

        # 8) Send audio data - handle send errors
        if not send_exact(conn, reply_bytes):
            print(f"[-] {client_ip}: Failed to send audio data")
            return

        print(f"[INFO] {client_ip}: Sent {mode_str} WAV ({len(reply_bytes)} bytes)")

    except socket.timeout:
        print(f"[-] {client_ip}: Socket timeout")
    except BrokenPipeError:
        print(f"[-] {client_ip}: Client disconnected (broken pipe)")
    except ConnectionResetError:
        print(f"[-] {client_ip}: Connection reset by peer")
    except Exception as e:
        print(f"[-] {client_ip}: Error handling connection: {e}")
    finally:
        try:
            conn.close()
        except:
            pass
        print(f"[x] {client_ip}: Disconnected")

# === Performance Optimized Connection Handler ===
def handle_connection_improved(conn, addr):
    """Handle a client connection with improved bidirectional communication and performance"""
    client_ip = f"{addr[0]}:{addr[1]}"
    print(f"[+] Connected: {client_ip}")

    try:
        # Optimize socket parameters for our use case
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # Disable Nagle algorithm

        # Perform handshake
        if not handle_handshake(conn, addr):
            print(f"[-] {client_ip}: Handshake failed")
            return

        # Receive audio data with verification
        print(f"[+] {client_ip}: Receiving audio data...")
        start_time = time.time()
        received_data = receive_data_with_verification(conn, addr)

        if received_data is None:
            print(f"[-] {client_ip}: Failed to receive audio data")
            return

        transfer_time = time.time() - start_time
        transfer_rate = len(received_data) / transfer_time / 1024 if transfer_time > 0 else 0
        print(f"[+] {client_ip}: Received {len(received_data)} bytes in {transfer_time:.2f}s ({transfer_rate:.1f} KB/s)")

        # Save received data
        os.makedirs("inbox", exist_ok=True)
        timestamp = time.strftime("%Y%m%d-%H%M%S")
        input_filename = f"inbox/{timestamp}_{addr[0]}.wav"

        with open(input_filename, "wb") as f:
            f.write(received_data)
        print(f"[+] {client_ip}: Saved {input_filename} ({len(received_data)} bytes)")

        # Verify it's a valid WAV
        if not verify_wav_format(received_data):
            print(f"[WARN] {client_ip}: Received data is not a valid WAV file")
            # Try to fix it
            received_data = ensure_valid_wav_format(received_data)

        # Process as before (transcribe, etc.)
        text = transcribe_wav_bytes(received_data)
        if text:
            print(f"[STT] {client_ip} said: \"{text}\"")
            if use_tts:
                speak_async(text)
        else:
            print(f"[STT] {client_ip}: No text recognized or STT disabled")

        # Prepare reply
        if ECHO_MODE:
            reply_bytes = process_audio(received_data)
            mode_str = "ECHO"
        else:
            if os.path.isfile(REPLY_WAV):
                with open(REPLY_WAV, "rb") as f:
                    reply_bytes = f.read()
                mode_str = "REPLY"
            else:
                print(f"[-] Reply WAV missing: {REPLY_WAV}")
                reply_bytes = make_silent_wav(sr=16000, ms=250)
                mode_str = "SILENT"

        # Ensure the WAV format is valid before sending
        if not verify_wav_format(reply_bytes):
            print(f"[WARN] Reply WAV format is invalid, repairing...")
            reply_bytes = ensure_valid_wav_format(reply_bytes)

        # Send reply using improved protocol
        print(f"[+] {client_ip}: Sending {mode_str} WAV ({len(reply_bytes)} bytes)")
        if not send_data_with_retries(conn, addr, reply_bytes):
            print(f"[-] {client_ip}: Failed to send reply")
            return

        print(f"[+] {client_ip}: Finished communication successfully")

    except socket.timeout:
        print(f"[-] {client_ip}: Socket timeout")
    except BrokenPipeError:
        print(f"[-] {client_ip}: Client disconnected (broken pipe)")
    except ConnectionResetError:
        print(f"[-] {client_ip}: Connection reset by peer")
    except Exception as e:
        print(f"[-] {client_ip}: Error handling connection: {e}")
    finally:
        # Clean up client state
        if client_ip in client_sequences:
            del client_sequences[client_ip]

        try:
            conn.close()
        except:
            pass
        print(f"[x] {client_ip}: Disconnected")

def main():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='WAV Echo Test Server')
    parser.add_argument('--host', default=HOST, help=f'Host to bind to (default: {HOST})')
    parser.add_argument('--port', type=int, default=PORT, help=f'Port to listen on (default: {PORT})')
    parser.add_argument('--model', default=MODEL_PATH, help=f'Path to Vosk model (default: {MODEL_PATH})')
    parser.add_argument('--reply', default=REPLY_WAV, help=f'Path to reply WAV file (default: {REPLY_WAV})')
    parser.add_argument('--echo', action='store_true', help='Echo mode: send back the received audio (default)')
    parser.add_argument('--no-echo', action='store_false', dest='echo',
                        help='Reply mode: send back the stock WAV file')
    parser.add_argument('--create-test-wav', action='store_true', help='Create a test reply.wav with a 440Hz tone')

    args = parser.parse_args()

    # Update global variables
    current_module = sys.modules[__name__]
    setattr(current_module, 'ECHO_MODE', args.echo)
    setattr(current_module, 'MODEL_PATH', args.model)
    setattr(current_module, 'REPLY_WAV', args.reply)

    # Create test WAV if requested
    if args.create_test_wav:
        create_test_wav()

    mode_str = "ECHO" if ECHO_MODE else "REPLY"
    print(f"[CONFIG] Server mode: {mode_str}")
    print(f"[CONFIG] Vosk model: {MODEL_PATH if use_vosk else 'disabled'}")
    print(f"[CONFIG] Reply WAV: {REPLY_WAV if not ECHO_MODE else 'N/A'}")

    # Check reply WAV in reply mode
    if not ECHO_MODE and not os.path.isfile(REPLY_WAV):
        print(f"[WARN] Reply WAV not found at {REPLY_WAV}. Silent WAV will be used as fallback.")

    # Create and configure socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((args.host, args.port))
        except Exception as e:
            print(f"[ERROR] Failed to bind to {args.host}:{args.port}: {e}")
            sys.exit(1)

        s.listen(5)
        print(f"[READY] Listening on {args.host}:{args.port}")

        try:
            while True:
                conn, addr = s.accept()
                # Set a reasonable timeout for the socket
                conn.settimeout(30.0)  # 30 second timeout
                # Handle each connection in a separate thread
                threading.Thread(target=handle_connection, args=(conn, addr), daemon=True).start()
        except KeyboardInterrupt:
            print("\n[EXIT] Server stopped by user")
        except Exception as e:
            print(f"[ERROR] Server error: {e}")
        finally:
            print("[EXIT] Shutting down server")

def main_improved():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description='WAV Echo Test Server (Improved Protocol)')
    parser.add_argument('--host', default=HOST, help=f'Host to bind to (default: {HOST})')
    parser.add_argument('--port', type=int, default=PORT, help=f'Port to listen on (default: {PORT})')
    parser.add_argument('--model', default=MODEL_PATH, help=f'Path to Vosk model (default: {MODEL_PATH})')
    parser.add_argument('--reply', default=REPLY_WAV, help=f'Path to reply WAV file (default: {REPLY_WAV})')
    parser.add_argument('--echo', action='store_true', help='Echo mode: send back the received audio (default)')
    parser.add_argument('--no-echo', action='store_false', dest='echo',
                        help='Reply mode: send back the stock WAV file')
    parser.add_argument('--create-test-wav', action='store_true', help='Create a test reply.wav with a 440Hz tone')

    args = parser.parse_args()

    # Update global variables
    current_module = sys.modules[__name__]
    setattr(current_module, 'ECHO_MODE', args.echo)
    setattr(current_module, 'MODEL_PATH', args.model)
    setattr(current_module, 'REPLY_WAV', args.reply)

    # Create test WAV if requested
    if args.create_test_wav:
        create_test_wav()

    mode_str = "ECHO" if ECHO_MODE else "REPLY"
    print(f"[CONFIG] Server mode: {mode_str}")
    print(f"[CONFIG] Vosk model: {MODEL_PATH if use_vosk else 'disabled'}")
    print(f"[CONFIG] Reply WAV: {REPLY_WAV if not ECHO_MODE else 'N/A'}")

    # Check reply WAV in reply mode
    if not ECHO_MODE and not os.path.isfile(REPLY_WAV):
        print(f"[WARN] Reply WAV not found at {REPLY_WAV}. Silent WAV will be used as fallback.")

    # Create and configure socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((args.host, args.port))
        except Exception as e:
            print(f"[ERROR] Failed to bind to {args.host}:{args.port}: {e}")
            sys.exit(1)

        s.listen(5)
        print(f"[READY] Listening on {args.host}:{args.port} (Improved Protocol)")

        try:
            while True:
                conn, addr = s.accept()
                # Set a reasonable timeout for the socket
                conn.settimeout(30.0)  # 30 second timeout
                # Handle each connection in a separate thread
                threading.Thread(target=handle_connection_improved, args=(conn, addr), daemon=True).start()
        except KeyboardInterrupt:
            print("\n[EXIT] Server stopped by user")
        except Exception as e:
            print(f"[ERROR] Server error: {e}")
        finally:
            print("[EXIT] Shutting down server")

if __name__ == "__main__":
    # Uncomment one of these:
    # main()  # Original protocol
    main_improved()  # Improved protocol
