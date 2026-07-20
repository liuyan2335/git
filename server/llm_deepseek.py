#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
llm_deepseek.py - DeepSeek Large Language Model API Client

Encapsulates the DeepSeek API for multi-turn conversational AI.
Supports streaming responses, context history management,
and configurable system prompts for different use cases.

API Reference: https://platform.deepseek.com/api-docs

Prerequisites:
    1. Register at https://platform.deepseek.com/
    2. Create an API key
    3. pip install requests

Usage:
    from llm_deepseek import DeepSeekChat

    chat = DeepSeekChat(api_key="your_key")
    response = chat.chat("你好，介绍一下你自己")
    print(response)

    # Multi-turn conversation
    history = [
        {"role": "user", "content": "今天天气怎么样？"},
        {"role": "assistant", "content": "抱歉，我无法获取实时天气信息。"},
    ]
    response = chat.chat("那你能做什么？", history=history)
"""

import os
import sys
import json
import time
import logging
from typing import Optional, List, Dict, Generator

log = logging.getLogger("llm_deepseek")

# ──────────────────────────────────────────────
#  Default Configuration
# ──────────────────────────────────────────────

DEFAULT_MODEL      = "deepseek-chat"
DEFAULT_BASE_URL   = "https://api.deepseek.com"
DEFAULT_MAX_TOKENS = 2048
DEFAULT_TEMPERATURE = 0.7
DEFAULT_TIMEOUT    = 30  # seconds

# System prompt for the RK3568 voice assistant
SYSTEM_PROMPT_VOICE_ASSISTANT = (
    "你是一个运行在嵌入式RK3568开发板上的智能语音助手。"
    "你通过触摸屏和麦克风与用户交互，用户的提问经过语音识别转写。"
    "请用简洁、自然、口语化的中文回答。"
    "每次回答控制在100字以内，方便在LCD屏幕上显示。"
    "如果用户的问题不清晰（可能是语音识别错误），请礼貌地请用户重说一遍。"
    "对于无法回答的问题，如实说明，不要编造信息。"
)

SYSTEM_PROMPT_GENERAL = (
    "你是一个乐于助人的AI助手。请用中文回答用户的问题。"
    "回答要准确、简洁、有条理。"
)


class DeepSeekChat:
    """DeepSeek API chat client with multi-turn conversation support.

    Features:
        - Multi-turn dialogue with history management
        - Streaming and non-streaming response modes
        - Customizable system prompt
        - Automatic retry on transient errors
        - Token usage tracking

    Example:
        chat = DeepSeekChat(api_key="sk-xxx")
        response = chat.chat("你好")
        print(response)
    """

    def __init__(self,
                 api_key: str = "",
                 model: str = DEFAULT_MODEL,
                 base_url: str = DEFAULT_BASE_URL,
                 system_prompt: str = "",
                 temperature: float = DEFAULT_TEMPERATURE,
                 max_tokens: int = DEFAULT_MAX_TOKENS,
                 timeout: int = DEFAULT_TIMEOUT):
        """
        Initialize the DeepSeek chat client.

        Args:
            api_key:       DeepSeek API key (with "sk-" prefix)
            model:         Model name (default: deepseek-chat)
            base_url:      API base URL
            system_prompt: Default system prompt for conversations
            temperature:   Sampling temperature (0.0-2.0)
            max_tokens:    Maximum tokens in response
            timeout:       HTTP request timeout in seconds
        """
        self.api_key = api_key
        self.model = model
        self.base_url = base_url.rstrip("/")
        self.system_prompt = system_prompt or SYSTEM_PROMPT_VOICE_ASSISTANT
        self.temperature = temperature
        self.max_tokens = max_tokens
        self.timeout = timeout

        # Full API endpoint
        self.api_url = f"{self.base_url}/v1/chat/completions"

        # Token usage tracking
        self.total_tokens_used = 0
        self.total_requests = 0

        # Check configuration
        if not api_key:
            log.warning("DeepSeekChat: No API key provided. "
                        "Set DEEPSEEK_API_KEY env var or pass api_key parameter.")

    def _build_payload(self, user_message: str,
                       history: Optional[List[Dict[str, str]]] = None,
                       system_prompt: Optional[str] = None,
                       stream: bool = False) -> dict:
        """Build the JSON request payload for the DeepSeek API.

        Args:
            user_message:  The user's input text
            history:       Previous conversation turns
            system_prompt: Override the default system prompt
            stream:        Enable streaming response

        Returns:
            Full request payload dict
        """
        messages = []

        # Add system prompt
        sys_prompt = system_prompt if system_prompt is not None else self.system_prompt
        if sys_prompt:
            messages.append({"role": "system", "content": sys_prompt})

        # Add conversation history
        if history:
            for turn in history:
                if "role" in turn and "content" in turn:
                    messages.append({
                        "role": turn["role"],
                        "content": turn["content"],
                    })

        # Add current user message
        if user_message:
            messages.append({"role": "user", "content": user_message})

        payload = {
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
            "stream": stream,
        }

        return payload

    def _make_request(self, payload: dict) -> Optional[dict]:
        """Send a request to the DeepSeek API and return the JSON response.

        Handles authentication, errors, and retries.

        Args:
            payload: Full request payload

        Returns:
            Response JSON dict, or None on failure.
        """
        try:
            import requests
        except ImportError:
            log.error("requests library not installed. Run: pip install requests")
            return None

        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.api_key}",
        }

        # Retry on transient errors (up to 3 attempts)
        max_retries = 3
        for attempt in range(max_retries):
            try:
                response = requests.post(
                    self.api_url,
                    headers=headers,
                    json=payload,
                    timeout=self.timeout,
                )

                if response.status_code == 200:
                    data = response.json()
                    self.total_requests += 1
                    # Track token usage
                    usage = data.get("usage", {})
                    tokens = usage.get("total_tokens", 0)
                    self.total_tokens_used += tokens
                    return data

                elif response.status_code == 401:
                    log.error("DeepSeek API: Authentication failed. Check your API key.")
                    try:
                        err = response.json()
                        log.error(f"  Detail: {err}")
                    except Exception:
                        pass
                    return None

                elif response.status_code == 429:
                    # Rate limited — exponential backoff
                    wait = 2 ** attempt
                    log.warning(f"DeepSeek API: Rate limited, retrying in {wait}s...")
                    time.sleep(wait)
                    continue

                elif response.status_code == 503:
                    # Service unavailable
                    wait = 2 ** attempt
                    log.warning(f"DeepSeek API: Service unavailable, retrying in {wait}s...")
                    time.sleep(wait)
                    continue

                else:
                    log.error(f"DeepSeek API: HTTP {response.status_code}")
                    try:
                        log.error(f"  Response: {response.text[:500]}")
                    except Exception:
                        pass

                    # Retry on server errors (5xx)
                    if response.status_code >= 500:
                        continue
                    return None

            except requests.exceptions.Timeout:
                log.warning(f"DeepSeek API: Timeout (attempt {attempt + 1}/{max_retries})")
                if attempt < max_retries - 1:
                    time.sleep(2)
                    continue
                return None

            except requests.exceptions.ConnectionError as e:
                log.warning(f"DeepSeek API: Connection error (attempt {attempt + 1}/{max_retries}): {e}")
                if attempt < max_retries - 1:
                    time.sleep(2)
                    continue
                return None

            except Exception as e:
                log.error(f"DeepSeek API: Unexpected error: {e}")
                return None

        log.error(f"DeepSeek API: All {max_retries} attempts failed.")
        return None

    def chat(self, user_message: str,
             history: Optional[List[Dict[str, str]]] = None,
             system_prompt: Optional[str] = None) -> Optional[str]:
        """Send a message to DeepSeek and get the AI response.

        This is the main public API. Supports multi-turn conversations
        by passing previous turns in the `history` parameter.

        Args:
            user_message:  User's message text (Chinese or English)
            history:       Previous turns: [{"role":"user","content":"..."},
                           {"role":"assistant","content":"..."}]
            system_prompt: Override default system prompt for this request

        Returns:
            AI response text string, or None if the call failed.
        """
        if not self.api_key:
            log.error("No API key configured. Cannot call DeepSeek API.")
            return None

        if not user_message or not user_message.strip():
            log.warning("Empty user message, skipping API call.")
            return None

        log.info(f"DeepSeek chat request ({len(user_message)} chars): {user_message[:80]}...")

        payload = self._build_payload(user_message, history, system_prompt, stream=False)

        result = self._make_request(payload)
        if not result:
            return None

        # Extract the assistant's reply
        try:
            choices = result.get("choices", [])
            if not choices:
                log.warning("DeepSeek API returned no choices.")
                return None

            message = choices[0].get("message", {})
            content = message.get("content", "")

            if not content:
                log.warning("DeepSeek API returned empty content.")
                return None

            if log.isEnabledFor(logging.DEBUG):
                finish_reason = choices[0].get("finish_reason", "unknown")
                usage = result.get("usage", {})
                log.debug(f"  Finish: {finish_reason}, Tokens: {usage.get('total_tokens', '?')}")

            log.info(f"DeepSeek response ({len(content)} chars): {content[:80]}...")
            return content

        except (KeyError, IndexError, TypeError) as e:
            log.error(f"Failed to parse DeepSeek response: {e}")
            return None

    def chat_stream(self, user_message: str,
                    history: Optional[List[Dict[str, str]]] = None,
                    system_prompt: Optional[str] = None) -> Generator[str, None, None]:
        """Send a message and get streaming response chunks.

        Yields text fragments as they arrive from the API.
        Useful for displaying responses in real-time on the LCD screen.

        Args:
            user_message:  User's message text
            history:       Previous conversation turns
            system_prompt: Override default system prompt

        Yields:
            String fragments of the AI response.
        """
        try:
            import requests
        except ImportError:
            log.error("requests library not installed.")
            return

        if not self.api_key or not user_message:
            return

        payload = self._build_payload(user_message, history, system_prompt, stream=True)

        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.api_key}",
        }

        try:
            response = requests.post(
                self.api_url,
                headers=headers,
                json=payload,
                timeout=self.timeout,
                stream=True,
            )

            if response.status_code != 200:
                log.error(f"DeepSeek streaming error: HTTP {response.status_code}")
                try:
                    err = response.json()
                    log.error(f"  Detail: {err}")
                except Exception:
                    pass
                return

            for line in response.iter_lines(decode_unicode=True):
                if not line or not line.startswith("data:"):
                    continue

                data_str = line[5:].strip()
                if data_str == "[DONE]":
                    break

                try:
                    chunk = json.loads(data_str)
                    choices = chunk.get("choices", [])
                    if choices:
                        delta = choices[0].get("delta", {})
                        content = delta.get("content", "")
                        if content:
                            yield content
                except json.JSONDecodeError:
                    continue

            self.total_requests += 1

        except requests.exceptions.Timeout:
            log.error("DeepSeek streaming timeout.")
        except Exception as e:
            log.error(f"DeepSeek streaming error: {e}")

    def count_tokens(self, text: str) -> int:
        """Estimate token count for a text string.

        Uses a simple approximation: ~1.5 Chinese characters per token,
        ~4 English characters per token. For accurate counts, use
        the tiktoken library or the DeepSeek tokenizer API.

        Args:
            text: Input text string

        Returns:
            Estimated token count.
        """
        if not text:
            return 0

        # Simple heuristic estimation
        chinese_chars = sum(1 for c in text if '一' <= c <= '鿿')
        other_chars = len(text) - chinese_chars

        # Approximate: 1 Chinese char ≈ 1.5 tokens, 4 English chars ≈ 1 token
        estimated = int(chinese_chars * 1.5 + other_chars / 4)
        return max(1, estimated)

    def get_stats(self) -> dict:
        """Get usage statistics for this client.

        Returns:
            Dict with total_requests, total_tokens, etc.
        """
        return {
            "total_requests": self.total_requests,
            "total_tokens_used": self.total_tokens_used,
            "model": self.model,
        }

    def clear_stats(self):
        """Reset usage statistics."""
        self.total_requests = 0
        self.total_tokens_used = 0


# ──────────────────────────────────────────────
#  Standalone Test
# ──────────────────────────────────────────────

def main():
    """Interactive test of the DeepSeek chat module."""
    import argparse

    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    parser = argparse.ArgumentParser(description="DeepSeek Chat Test")
    parser.add_argument("--api-key", default=os.environ.get("DEEPSEEK_API_KEY", ""),
                        help="DeepSeek API key")
    parser.add_argument("--model", default=DEFAULT_MODEL, help="Model name")
    parser.add_argument("--stream", action="store_true", help="Use streaming mode")
    parser.add_argument("message", nargs="?", default="你好，介绍一下你自己",
                        help="Message to send")
    args = parser.parse_args()

    if not args.api_key:
        print("ERROR: Set DEEPSEEK_API_KEY environment variable or pass --api-key.")
        sys.exit(1)

    chat = DeepSeekChat(api_key=args.api_key, model=args.model)

    if args.stream:
        print("Streaming response:")
        print("-" * 40)
        full_response = ""
        for chunk in chat.chat_stream(args.message):
            print(chunk, end="", flush=True)
            full_response += chunk
        print()
        print("-" * 40)
        print(f"\nTotal: {len(full_response)} chars")
    else:
        response = chat.chat(args.message)
        if response:
            print(f"User: {args.message}")
            print(f"AI:   {response}")
        else:
            print("Chat failed.")
            sys.exit(1)


if __name__ == "__main__":
    main()
