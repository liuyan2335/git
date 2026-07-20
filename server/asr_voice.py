#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
asr_voice.py - Xunfei (iFlytek) Speech Recognition Wrapper

Integrates with Xunfei's WebSocket-based real-time ASR API.
Converts PCM/WAV audio files to Chinese text using the Xunfei
Speech-to-Text (语音听写) service.

Prerequisites:
    1. Register at https://console.xfyun.cn/
    2. Create a "语音听写" (Speech-to-Text) application
    3. Obtain AppID, APIKey, and APISecret

API Documentation:
    https://www.xfyun.cn/doc/asr/voicedictation/API.html

Usage:
    from asr_voice import XunfeiASR
    asr = XunfeiASR(app_id="xxx", api_key="xxx", api_secret="xxx")
    text = asr.recognize("audio.wav")  # returns transcribed Chinese text
"""

import os
import sys
import json
import time
import base64
import hashlib
import hmac
import logging
import tempfile
import subprocess
from pathlib import Path
from datetime import datetime
from urllib.parse import urlencode
from typing import Optional

log = logging.getLogger("asr_voice")

# ──────────────────────────────────────────────
#  Xunfei ASR WebSocket Configuration
# ──────────────────────────────────────────────

XF_ASR_HOST  = "iat-api.xfyun.cn"       # 语音听写 WebAPI 地址
XF_ASR_PATH  = "/v2/iat"                 # API 路径
XF_ASR_URL   = f"wss://{XF_ASR_HOST}{XF_ASR_PATH}"

# Audio format constants
AUDIO_FORMAT_PCM   = "raw"
AUDIO_FORMAT_WAV   = "wav"
AUDIO_ENCODING_16K = "audio/L16;rate=16000"
AUDIO_ENCODING_8K  = "audio/L16;rate=8000"


class XunfeiASR:
    """Xunfei (iFlytek) Speech-to-Text recognition engine.

    Uses WebSocket API for real-time speech recognition.
    Supports both file-based and streaming recognition modes.

    Example:
        asr = XunfeiASR(
            app_id="abc123",
            api_key="your_key_here",
            api_secret="your_secret_here",
        )
        text = asr.recognize("recording.wav")
        print(f"识别结果: {text}")
    """

    def __init__(self, app_id: str = "", api_key: str = "",
                 api_secret: str = "", host: str = XF_ASR_HOST):
        """
        Initialize the Xunfei ASR client.

        Args:
            app_id:     Xunfei application ID
            api_key:    Xunfei API key
            api_secret: Xunfei API secret
            host:       API host (default: iat-api.xfyun.cn)
        """
        self.app_id = app_id
        self.api_key = api_key
        self.api_secret = api_secret
        self.host = host
        self.path = XF_ASR_PATH
        self.url = f"wss://{host}{self.path}"

        # Check if credentials are configured
        if not all([app_id, api_key, api_secret]):
            log.warning("XunfeiASR: Missing credentials. "
                        "Set XF_APP_ID, XF_API_KEY, XF_API_SECRET env vars "
                        "or pass them directly.")

    def _build_auth_url(self) -> str:
        """Build the authenticated WebSocket URL with HMAC-SHA256 signature.

        Xunfei uses the following signature algorithm:
            1. Build the request line: host, date, GET, path
            2. Compute HMAC-SHA256 with api_secret
            3. Base64-encode the signature
            4. Build the authorization header
            5. Append as query params to the URL
        """
        # Generate RFC 1123 date
        from email.utils import formatdate
        date = formatdate(timeval=time.time(), localtime=False, usegmt=True)

        # Signature origin string
        signature_origin = f"host: {self.host}\ndate: {date}\nGET {self.path} HTTP/1.1"

        # HMAC-SHA256 signature
        signature_sha = hmac.new(
            self.api_secret.encode("utf-8"),
            signature_origin.encode("utf-8"),
            digestmod=hashlib.sha256,
        ).digest()
        signature = base64.b64encode(signature_sha).decode("utf-8")

        # Build authorization
        authorization_origin = (
            f'api_key="{self.api_key}", '
            f'algorithm="hmac-sha256", '
            f'headers="host date request-line", '
            f'signature="{signature}"'
        )
        authorization = base64.b64encode(
            authorization_origin.encode("utf-8")
        ).decode("utf-8")

        # Build query params
        query = urlencode({
            "authorization": authorization,
            "date": date,
            "host": self.host,
        })

        return f"{self.url}?{query}"

    def _websocket_recognize(self, audio_data: bytes,
                              audio_format: str = AUDIO_ENCODING_16K,
                              language: str = "zh_cn",
                              timeout: int = 30) -> Optional[str]:
        """Send audio data to Xunfei ASR via WebSocket and get transcription.

        This is the core recognition method using the websocket protocol.
        Falls back to HTTP REST API if websocket-client is not available.

        Args:
            audio_data:   Raw PCM audio bytes (16kHz, 16-bit, mono)
            audio_format: Audio encoding format string
            language:     Recognition language (zh_cn, en_us, etc.)
            timeout:      Connection timeout in seconds

        Returns:
            Transcribed text string, or None if recognition failed.
        """
        try:
            import websocket
        except ImportError:
            log.warning("websocket-client not installed, trying REST API fallback.")
            log.warning("Install with: pip install websocket-client")
            return self._rest_fallback(audio_data)

        # Split audio into frames (Xunfei expects streaming chunks)
        frame_size = 1280  # 40ms at 16kHz 16-bit mono
        frames = [
            audio_data[i:i + frame_size]
            for i in range(0, len(audio_data), frame_size)
        ]

        result_text = ""

        ws_url = self._build_auth_url()
        ws = websocket.WebSocket()
        ws.settimeout(timeout)

        try:
            ws.connect(ws_url)

            # Phase 1: Send parameters
            param_data = {
                "common": {
                    "app_id": self.app_id,
                },
                "business": {
                    "language": language,
                    "domain": "iat",
                    "accent": "mandarin",
                    "dwa": "wpgs",       # dynamic word-level confidence
                    "pd": "game",        # personalized dict
                    "vad_eos": 10000,    # VAD end-of-speech timeout (ms)
                },
                "data": {
                    "status": 0,         # 0 = first frame
                    "format": audio_format,
                    "encoding": "raw",
                    "audio": base64.b64encode(frames[0] if frames else b"").decode("utf-8"),
                },
            }
            ws.send(json.dumps(param_data))

            # Phase 2: Send remaining audio frames
            for i, frame in enumerate(frames):
                if i == 0:
                    continue  # Already sent first frame

                frame_data = {
                    "data": {
                        "status": 1 if i < len(frames) - 1 else 2,  # 1=continuing, 2=last
                        "audio": base64.b64encode(frame).decode("utf-8"),
                    },
                }
                ws.send(json.dumps(frame_data))

            # Phase 3: Receive results
            while True:
                try:
                    msg = ws.recv()
                    if not msg:
                        break

                    response = json.loads(msg)
                    code = response.get("code", 0)

                    if code != 0:
                        err_msg = response.get("message", f"Error code {code}")
                        log.error(f"Xunfei API error: {err_msg}")
                        break

                    data = response.get("data", {})
                    result = data.get("result", {})

                    if not result:
                        continue

                    # Concatenate text fragments
                    ws_status = result.get("ls", False)  # last sentence flag
                    text = ""

                    for seg in result.get("ws", []):
                        for word in seg.get("cw", []):
                            text += word.get("w", "")

                    result_text += text

                    if data.get("status") == 2 or ws_status:
                        break  # Complete

                except websocket.WebSocketTimeoutException:
                    log.warning("WebSocket timeout during recognition.")
                    break

        except websocket.WebSocketException as e:
            log.error(f"WebSocket error: {e}")
        except json.JSONDecodeError as e:
            log.error(f"JSON parse error: {e}")
        except Exception as e:
            log.error(f"Unexpected error in WebSocket recognize: {e}")
        finally:
            try:
                ws.close()
            except Exception:
                pass

        # Fallback if no WebSocket result
        if not result_text:
            return self._rest_fallback(audio_data)

        return result_text

    def _rest_fallback(self, audio_data: bytes) -> Optional[str]:
        """REST API fallback using Xunfei's HTTP interface.

        Uses the file-upload based REST endpoint when WebSocket is unavailable.
        More limited but doesn't require websocket-client dependency.
        """
        try:
            import requests
        except ImportError:
            log.error("requests library not installed. Install with: pip install requests")
            return None

        # Write audio to a temporary WAV file
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            # Write WAV header for 16kHz, 16-bit, mono PCM
            data_size = len(audio_data)
            import struct

            f.write(b"RIFF")
            f.write(struct.pack("<I", 36 + data_size))
            f.write(b"WAVE")
            f.write(b"fmt ")
            f.write(struct.pack("<I", 16))
            f.write(struct.pack("<H", 1))      # PCM
            f.write(struct.pack("<H", 1))      # mono
            f.write(struct.pack("<I", 16000))  # sample rate
            f.write(struct.pack("<I", 32000))  # byte rate
            f.write(struct.pack("<H", 2))      # block align
            f.write(struct.pack("<H", 16))     # bits per sample
            f.write(b"data")
            f.write(struct.pack("<I", data_size))
            f.write(audio_data)
            temp_wav = f.name

        try:
            # Build request parameters
            param = {
                "engine_type": "sms16k",    # 16kHz Mandarin
                "aue": "raw",               # return raw text
            }

            x_param = base64.b64encode(json.dumps(param).encode()).decode()

            # Current timestamp
            x_time = str(int(time.time()))

            # MD5 checksum: api_key + x_time + x_param
            checksum_str = self.api_key + x_time + x_param
            x_checksum = hashlib.md5(checksum_str.encode()).hexdigest()  # noqa: S324

            headers = {
                "X-Appid": self.app_id,
                "X-CurTime": x_time,
                "X-Param": x_param,
                "X-CheckSum": x_checksum,
                "Content-Type": "application/octet-stream",
            }

            with open(temp_wav, "rb") as f:
                response = requests.post(
                    "https://api.xfyun.cn/v1/service/v1/iat",
                    headers=headers,
                    data=f.read(),
                    timeout=30,
                )

            if response.status_code == 200:
                result = response.json()
                if result.get("code") == "0":
                    return result.get("data", {}).get("result", "")
                else:
                    log.error(f"Xunfei REST error: {result.get('desc', result)}")
                    return None
            else:
                log.error(f"Xunfei REST HTTP error: {response.status_code}")
                return None

        finally:
            # Clean up temp file
            try:
                os.unlink(temp_wav)
            except OSError:
                pass

    def recognize(self, audio_path: str,
                   language: str = "zh_cn",
                   timeout: int = 30) -> Optional[str]:
        """Recognize speech from an audio file.

        This is the main public API. Reads the audio file and sends it
        to Xunfei's ASR service for transcription.

        Args:
            audio_path: Path to a WAV or raw PCM audio file
            language:   Recognition language code (default: zh_cn)
            timeout:    API call timeout in seconds

        Returns:
            Transcribed Chinese text string, or None on failure.
        """
        audio_file = Path(audio_path)

        if not audio_file.exists():
            log.error(f"Audio file not found: {audio_path}")
            return None

        file_size = audio_file.stat().st_size
        if file_size == 0:
            log.error(f"Audio file is empty: {audio_path}")
            return None

        log.info(f"Recognizing audio: {audio_path} ({file_size} bytes)")

        try:
            # Read the audio file
            with open(audio_path, "rb") as f:
                raw_data = f.read()

            # Strip WAV header if present (first 44 bytes for standard WAV)
            if raw_data[:4] == b"RIFF" and len(raw_data) > 44:
                log.debug("Detected WAV format, skipping header.")
                # Find the "data" chunk
                data_offset = raw_data.find(b"data")
                if data_offset > 0:
                    # Skip "data" marker (4 bytes) + size field (4 bytes)
                    pcm_data = raw_data[data_offset + 8:]
                    log.debug(f"Extracted {len(pcm_data)} bytes of PCM data.")
                else:
                    # Fallback: skip standard 44-byte header
                    pcm_data = raw_data[44:]
            else:
                pcm_data = raw_data

            if not pcm_data:
                log.error("No audio data after header stripping.")
                return None

            # Limit to 60 seconds of audio (16kHz * 2 bytes * 60)
            max_bytes = 16000 * 2 * 60
            if len(pcm_data) > max_bytes:
                log.warning(f"Audio too long ({len(pcm_data)} bytes), truncating to 60s.")
                pcm_data = pcm_data[:max_bytes]

            # Run recognition
            result = self._websocket_recognize(pcm_data, language=language, timeout=timeout)

            if result:
                log.info(f"Recognition result ({len(result)} chars): {result[:80]}...")
            else:
                log.warning("Recognition returned no result.")

            return result

        except IOError as e:
            log.error(f"Failed to read audio file: {e}")
            return None
        except Exception as e:
            log.error(f"Recognition error: {e}")
            return None

    def recognize_from_bytes(self, pcm_data: bytes,
                              language: str = "zh_cn",
                              timeout: int = 30) -> Optional[str]:
        """Recognize speech from raw PCM bytes in memory.

        Useful when streaming audio directly without file I/O.

        Args:
            pcm_data: Raw 16kHz 16-bit mono PCM audio bytes
            language: Recognition language code
            timeout:  API call timeout in seconds

        Returns:
            Transcribed text string, or None on failure.
        """
        if not pcm_data:
            log.error("Empty audio data provided.")
            return None

        return self._websocket_recognize(pcm_data, language=language, timeout=timeout)


# ──────────────────────────────────────────────
#  Standalone Test
# ──────────────────────────────────────────────

def main():
    """Quick test of the ASR module."""
    import argparse

    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    parser = argparse.ArgumentParser(description="Xunfei ASR Test")
    parser.add_argument("audio_file", help="Path to WAV audio file (16kHz, 16-bit, mono)")
    parser.add_argument("--app-id", default=os.environ.get("XF_APP_ID", ""), help="Xunfei App ID")
    parser.add_argument("--api-key", default=os.environ.get("XF_API_KEY", ""), help="Xunfei API Key")
    parser.add_argument("--api-secret", default=os.environ.get("XF_API_SECRET", ""), help="Xunfei API Secret")
    args = parser.parse_args()

    if not all([args.app_id, args.api_key, args.api_secret]):
        print("ERROR: Set XF_APP_ID, XF_API_KEY, XF_API_SECRET environment variables.")
        print("  Or pass --app-id, --api-key, --api-secret arguments.")
        sys.exit(1)

    asr = XunfeiASR(
        app_id=args.app_id,
        api_key=args.api_key,
        api_secret=args.api_secret,
    )

    print(f"Transcribing: {args.audio_file}")
    result = asr.recognize(args.audio_file)

    if result:
        print(f"\n识别结果: {result}")
    else:
        print("识别失败，请检查日志。")
        sys.exit(1)


if __name__ == "__main__":
    main()
