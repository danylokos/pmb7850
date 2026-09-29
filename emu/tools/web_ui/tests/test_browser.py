#!/usr/bin/env python3

import asyncio
import contextlib
import json
import os
import struct
import tempfile
import time
import unittest
from pathlib import Path

from playwright.async_api import async_playwright

from .. import app as web_ui
from . import CEMU_ROOT
from .test_server import C55_BUTTONS, M55_BUTTONS, FakeCemu, wait_until


def chrome_executable():
    override = os.environ.get("CEMU_CHROME_PATH")
    if override:
        return override
    candidates = (
        "/usr/bin/google-chrome",
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    )
    for candidate in candidates:
        if Path(candidate).is_file():
            return candidate
    return None


async def launch_browser(playwright):
    name = os.environ.get("CEMU_TEST_BROWSER", "chromium")
    if name == "firefox":
        return await playwright.firefox.launch(headless=True)
    if name != "chromium":
        raise ValueError(f"unsupported CEMU_TEST_BROWSER: {name}")
    return await playwright.chromium.launch(
        headless=True, executable_path=chrome_executable())


class BrowserUITests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cemu-browser-test-")
        self.path = os.path.join(self.temp.name, "cemu.sock")
        frame = bytes([40, 60, 35]) * (101 * 40) + bytes([190, 205, 155]) * (101 * 40)
        self.fake = FakeCemu(self.path, frame=frame, model="m55", width=101,
                             height=80, keys=M55_BUTTONS,
                             audio_available=True)
        await self.fake.start()
        self.bridge = web_ui.CemuBridge(self.path, reconnect_delay=0.03)
        self.bridge_task = asyncio.create_task(self.bridge.run())
        await asyncio.wait_for(self.fake.connected.wait(), 2)
        await wait_until(lambda: self.bridge.frame is not None)
        self.web = web_ui.WebServer(self.bridge, "127.0.0.1", 0)
        await self.web.start()
        self.playwright = await async_playwright().start()
        self.browser = await launch_browser(self.playwright)
        self.context = await self.browser.new_context(viewport={"width": 1280, "height": 900})
        await self.context.add_init_script("""(() => {
          window.__copiedConsole = null;
          Object.defineProperty(navigator, "clipboard", {
            configurable: true,
            value: {writeText: async text => { window.__copiedConsole = text; }},
          });
          window.__audioMessages = [];
          window.__audioResumeCount = 0;
          window.AudioContext = class {
            constructor(options) {
              this.state = "running";
              this.sampleRate = options.sampleRate;
              this.destination = {};
              this.audioWorklet = {addModule: async () => {}};
              window.__lastAudioContext = this;
            }
            async resume() {
              window.__audioResumeCount++;
              this.state = "running";
            }
            async close() { this.state = "closed"; }
          };
          window.AudioWorkletNode = class {
            constructor() {
              this.port = {postMessage: message => window.__audioMessages.push({
                type: message.type,
                bytes: message.samples?.byteLength || 0,
              })};
            }
            connect() {}
          };
        })()""")
        self.page = await self.context.new_page()
        self.console_errors = []
        self.page.on("console", lambda message: self.console_errors.append(message.text)
                     if message.type == "error" else None)
        self.page.on("pageerror", lambda error: self.console_errors.append(str(error)))
        await self.page.goto(f"http://127.0.0.1:{self.web.port}/")
        await self.page.locator("#connection-state.connected").wait_for()

    async def asyncTearDown(self):
        await self.context.close()
        await self.browser.close()
        await self.playwright.stop()
        await self.web.stop()
        await self.bridge.stop()
        self.bridge_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await self.bridge_task
        await self.fake.stop()
        self.temp.cleanup()

    async def next_control(self):
        return await asyncio.wait_for(self.fake.received.get(), 2)

    async def test_successive_samples_display_service_rates(self):
        for seq, tick_rate, guest_rate in ((2, 10_000_000, 8_000_000), (3, 1_000_000, 500_000)):
            self.fake.writer.write(web_ui.encode_packet(
                web_ui.STATS, web_ui.STATS_PAYLOAD.pack(2_000_000_000, 11_000_000,
                    8_500_000, 0x123456, seq, time.monotonic_ns(),
                    1_000_000_000, tick_rate, guest_rate, 1)))
            await self.fake.writer.drain()
            await self.page.wait_for_function(
                "text => document.querySelector('#runtime-tick-rate').textContent === text",
                arg=f"{tick_rate / 1e6:.1f}M/s")
            self.assertEqual(await self.page.locator("#runtime-guest-rate").inner_text(),
                             f"{guest_rate / 1e6:.1f}M/s")

    async def test_speaker_pcm_dispatch_off_ui_thread_and_reset(self):
        await self.page.dispatch_event("body", "pointerdown")

        pcm = struct.pack("<IHHhhhh", 48_000, 1, 0,
                          -32768, -1, 0, 32767)
        self.fake.writer.write(web_ui.encode_packet(
            web_ui.SPEAKER_PCM, pcm, sequence=214, icount=43_000_000))
        await self.fake.writer.drain()
        await asyncio.sleep(0.1)
        messages = await self.page.evaluate("window.__audioMessages")
        self.assertIn({"type": "pcm", "bytes": 8}, messages)

        self.fake.writer.write(web_ui.encode_packet(
            web_ui.AUDIO_RESET, sequence=230, icount=43_000_001))
        await self.fake.writer.drain()
        await asyncio.sleep(0.1)
        messages = await self.page.evaluate("window.__audioMessages")
        self.assertEqual(messages[-1]["type"], "reset")
        await self.page.evaluate("window.__lastAudioContext.state = 'suspended'")
        await self.page.dispatch_event("body", "keydown")
        await asyncio.sleep(0.05)
        self.assertEqual(await self.page.evaluate("window.__audioResumeCount"), 1)
        self.assertEqual(await self.page.evaluate(
            "window.__lastAudioContext.state"), "running")
        self.assertEqual(self.console_errors, [])

    async def test_audio_activation_discards_stale_pending_pcm(self):
        result = await self.page.evaluate("""async () => {
          const {createAudio} = await import('/audio.js');
          const audio = createAudio();
          let clock = 0;
          Object.defineProperty(performance, 'now', {value: () => clock});
          audio.setAvailable(true);
          audio.dispatchEnvelope(new Uint8Array([3, 1, 0, 2, 0]).buffer);
          clock = 150;
          audio.dispatchEnvelope(new Uint8Array([3, 1, 0, 2, 0, 3, 0]).buffer);
          document.dispatchEvent(new Event('pointerdown'));
          await new Promise(resolve => setTimeout(resolve, 0));
          return window.__audioMessages.filter(message => message.type === 'pcm');
        }""")
        self.assertEqual(result, [{"type": "pcm", "bytes": 6}])

    async def test_audio_worklet_fifo_bounds_underrun_and_reset(self):
        await self.page.evaluate("""() => {
          window.AudioWorkletProcessor = class {
            constructor() { this.port = {onmessage: null, postMessage: () => {}}; }
          };
          window.sampleRate = 48000;
          window.currentFrame = 0;
          window.registerProcessor = (_name, implementation) => {
            window.__CemuAudioProcessor = implementation;
          };
        }""")
        await self.page.add_script_tag(
            url=f"http://127.0.0.1:{self.web.port}/audio-worklet.js")
        cases = Path(__file__).with_name("audio_worklet_cases.js").read_text()
        result = await self.page.evaluate(cases)
        self.assertEqual([row["speed"] for row in result["rates"]], [0.5, 1, 2, 5, "changing"])
        for row in result["rates"]:
            self.assertEqual(row["sent"], row["consumed"])
        self.assertEqual(result["jitter"], [
            {"pauseMs": 80, "resumeFrame": 3968, "underruns": 0,
             "underrunFrames": 0, "recoveryWaitFrames": 0, "consumed": 5056},
            {"pauseMs": 120, "resumeFrame": 5888, "underruns": 1,
             "underrunFrames": 64, "recoveryWaitFrames": 4864, "consumed": 5056},
        ])

    async def test_audio_activation_age_boundary_and_pending_reset(self):
        result = await self.page.evaluate("""async () => {
          const {createAudio} = await import('/audio.js');
          const audio = createAudio();
          let clock = 0;
          Object.defineProperty(performance, 'now', {value: () => clock});
          audio.setAvailable(true);
          audio.dispatchEnvelope(new Uint8Array([3, 99, 0]).buffer);
          audio.dispatchEnvelope(new Uint8Array([2]).buffer);
          audio.dispatchEnvelope(new Uint8Array([3, 1, 0, 2, 0]).buffer);
          clock = 1;
          audio.dispatchEnvelope(new Uint8Array([3, 3, 0, 4, 0, 5, 0]).buffer);
          // No further arrivals: activation itself must reject the 101-ms packet.
          clock = 101;
          document.dispatchEvent(new Event('pointerdown'));
          await new Promise(resolve => setTimeout(resolve, 0));
          return window.__audioMessages.filter(message => message.type === 'pcm');
        }""")
        self.assertEqual(result, [{"type": "pcm", "bytes": 6}])

    async def test_audio_pending_reset_before_activation(self):
        result = await self.page.evaluate("""async () => {
          const {createAudio} = await import('/audio.js');
          const audio = createAudio();
          audio.setAvailable(true);
          audio.dispatchEnvelope(new Uint8Array([3, 99, 0]).buffer);
          audio.dispatchEnvelope(new Uint8Array([2]).buffer);
          audio.dispatchEnvelope(new Uint8Array([3, 1, 0, 2, 0]).buffer);
          document.dispatchEvent(new Event('pointerdown'));
          await new Promise(resolve => setTimeout(resolve, 0));
          return window.__audioMessages.filter(message => message.type === 'pcm');
        }""")
        self.assertEqual(result, [{"type": "pcm", "bytes": 4}])

    async def test_audio_reconnect_clears_output_without_replay(self):
        await self.page.dispatch_event("body", "pointerdown")
        pcm = struct.pack("<IHHhh", 48_000, 1, 0, -32768, 32767)
        self.fake.writer.write(web_ui.encode_packet(web_ui.SPEAKER_PCM, pcm))
        await self.fake.writer.drain()
        await self.page.wait_for_function(
            "() => window.__audioMessages.some(m => m.type === 'pcm')")
        await self.page.evaluate("window.__audioMessages = []")
        await self.fake.stop()
        await self.page.locator("#connection-state.error").wait_for()
        await self.page.wait_for_function(
            "() => window.__audioMessages.some(m => m.type === 'reset')")
        self.fake = FakeCemu(self.path, audio_available=True)
        await self.fake.start()
        await wait_until(lambda: self.bridge.connection_count >= 2)
        await self.page.locator("#connection-state.connected").wait_for()
        self.assertTrue(await self.page.evaluate(
            "window.__audioMessages.every(m => m.type === 'reset')"))
        self.fake.writer.write(web_ui.encode_packet(web_ui.SPEAKER_PCM, pcm))
        await self.fake.writer.drain()
        await self.page.wait_for_function(
            "() => window.__audioMessages.some(m => m.type === 'pcm')")
        self.assertEqual(await self.page.evaluate(
            "window.__audioMessages.filter(m => m.type === 'pcm')"),
            [{"type": "pcm", "bytes": 4}])
        self.assertEqual(self.console_errors, [])

    async def click_key(self, name):
        locator = self.page.locator(f'[data-key="{name}"]')
        box = await locator.bounding_box()
        self.assertIsNotNone(box)
        await self.page.mouse.move(box["x"] + box["width"] / 2,
                                   box["y"] + box["height"] / 2)
        await self.page.mouse.down()
        down = await self.next_control()
        await self.page.mouse.up()
        up = await self.next_control()
        return down, up

    async def assert_layout(self, metric_columns):
        result = await self.page.evaluate("""() => {
          const bounds = (element) => {
            const r = element.getBoundingClientRect();
            return {left: r.left, top: r.top, right: r.right, bottom: r.bottom,
                    width: r.width, height: r.height};
          };
          const phone = document.querySelector('.phone').getBoundingClientRect();
          const stats = document.querySelector('#runtime-stats').getBoundingClientRect();
          const directionElement = document.querySelector('.direction-pad');
          const directionPad = bounds(directionElement);
          const directionStyle = getComputedStyle(directionElement);
          const metrics = [...document.querySelectorAll('.runtime-metric')].map((element) => {
            const r = element.getBoundingClientRect();
            const children = [...element.children].map((child) => ({
              clientWidth: child.clientWidth, scrollWidth: child.scrollWidth,
              clientHeight: child.clientHeight, scrollHeight: child.scrollHeight,
            }));
            return {left: r.left, top: r.top, right: r.right, bottom: r.bottom,
                    width: r.width, height: r.height, children};
          });
          const keys = [...document.querySelectorAll('[data-key]:not([hidden])')].map((element) => {
            const r = element.getBoundingClientRect();
            return {name: element.dataset.key, left: r.left, top: r.top,
                    right: r.right, bottom: r.bottom, width: r.width, height: r.height};
          });
          const overlaps = [];
          // Firefox can report shared grid edges ~0.00005 CSS px apart.
          const overlapsArea = (a, b) =>
            Math.min(a.right, b.right) - Math.max(a.left, b.left) > 0.001 &&
            Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top) > 0.001;
          for (let i = 0; i < keys.length; i++) for (let j = i + 1; j < keys.length; j++) {
            const a = keys[i], b = keys[j];
            if (overlapsArea(a, b))
              overlaps.push([a.name, b.name]);
          }
          const metricOverlaps = [];
          for (let i = 0; i < metrics.length; i++) for (let j = i + 1; j < metrics.length; j++) {
            const a = metrics[i], b = metrics[j];
            if (overlapsArea(a, b))
              metricOverlaps.push([i, j]);
          }
          return {phone: {left: phone.left, right: phone.right, top: phone.top, bottom: phone.bottom},
                  stats: {left: stats.left, right: stats.right, width: stats.width},
                  directionPad, metrics, keys, overlaps, metricOverlaps,
                  hasHorizontalNav: document.querySelector('.phone').classList.contains('has-horizontal-nav'),
                  directionGrid: {columnGap: directionStyle.columnGap,
                                  rowGap: directionStyle.rowGap,
                                  centerContent: getComputedStyle(directionElement, '::before').content},
                  documentWidth: document.documentElement.scrollWidth,
                  viewport: {width: innerWidth, height: innerHeight}};
        }""")
        self.assertEqual(result["overlaps"], [])
        self.assertEqual(result["metricOverlaps"], [])
        self.assertEqual(len(result["keys"]), len(self.fake.keys))
        self.assertEqual(len(result["metrics"]), 5)
        self.assertGreaterEqual(result["phone"]["left"], 0)
        self.assertLessEqual(result["phone"]["right"], result["viewport"]["width"])
        self.assertLessEqual(result["documentWidth"], result["viewport"]["width"])
        for key in result["keys"]:
            self.assertGreater(key["width"], 20)
            self.assertGreater(key["height"], 20)

        keys = {key["name"]: key for key in result["keys"]}
        left_soft = keys["soft-left"]
        right_soft = keys["soft-right"]
        scale = left_soft["height"] / 22
        for softkey, action in ((left_soft, keys["send"]),
                                (right_soft, keys["power"])):
            self.assertAlmostEqual(softkey["left"], action["left"], delta=1)
            self.assertAlmostEqual(softkey["right"], action["right"], delta=1)
            self.assertAlmostEqual(softkey["width"], action["width"], delta=1)
            self.assertAlmostEqual(softkey["height"], 22 * scale, delta=1)
            self.assertAlmostEqual(action["height"], 34 * scale, delta=1)
            self.assertAlmostEqual(softkey["top"], result["directionPad"]["top"], delta=1)
            self.assertAlmostEqual(action["bottom"], result["directionPad"]["bottom"], delta=1)
            self.assertLess(softkey["bottom"], action["top"])
            self.assertGreater(result["directionPad"]["width"] * result["directionPad"]["height"],
                               softkey["width"] * softkey["height"])

        self.assertLess(left_soft["right"], result["directionPad"]["left"])
        self.assertLess(result["directionPad"]["right"], right_soft["left"])
        self.assertAlmostEqual(result["directionPad"]["left"] - left_soft["right"],
                               right_soft["left"] - result["directionPad"]["right"], delta=1)

        self.assertAlmostEqual(result["directionPad"]["height"], 78 * scale, delta=1)
        self.assertAlmostEqual(result["directionPad"]["width"],
                               (102 if result["hasHorizontalNav"] else 58) * scale,
                               delta=1)
        self.assertEqual(result["directionGrid"]["columnGap"], "0px")
        self.assertEqual(result["directionGrid"]["rowGap"], "0px")
        self.assertEqual(result["directionGrid"]["centerContent"], '\"\"')
        visible_directions = {name for name in ("up", "left", "right", "down")
                              if name in keys}
        self.assertEqual(visible_directions,
                         {"up", "left", "right", "down"}
                         if result["hasHorizontalNav"] else {"up", "down"})
        for metric in result["metrics"]:
            self.assertGreaterEqual(metric["height"], 39)
            for child in metric["children"]:
                self.assertLessEqual(child["scrollWidth"], child["clientWidth"])
                # Firefox rounds glyph extents and line-box height differently.
                self.assertLessEqual(child["scrollHeight"], child["clientHeight"] + 1)

        rows = {}
        for metric in result["metrics"]:
            rows.setdefault(round(metric["top"]), []).append(metric)
        self.assertEqual(sorted(len(row) for row in rows.values()), metric_columns)
        if metric_columns == [5]:
            self.assertTrue(all(
                abs(metric["width"] - result["stats"]["width"] / 5) < 1
                for metric in result["metrics"]
            ))
        else:
            pc = result["metrics"][-1]
            self.assertAlmostEqual(pc["left"], result["stats"]["left"], delta=1)
            self.assertAlmostEqual(pc["right"], result["stats"]["right"], delta=1)

    async def test_complete_interactive_phone(self):
        self.assertTrue(await self.page.locator("#asc0-panel").is_visible())
        self.assertEqual(await self.page.locator("#asc0-output").text_content(), "")
        self.assertTrue(await self.page.locator("body").evaluate(
            "body => body.classList.contains('asc0-visible')"
        ))
        self.assertTrue(await self.page.locator("#asc0-copy").is_disabled())
        self.assertEqual(
            await self.page.locator("#asc0-minimize").get_attribute("aria-expanded"),
            "true",
        )
        pixel = await self.page.locator("#lcd").evaluate("""canvas => {
          const p = canvas.getContext('2d').getImageData(0, 0, 1, 1).data;
          return [p[0], p[1], p[2], p[3]];
        }""")
        self.assertEqual(pixel, [40, 60, 35, 255])
        await self.assert_layout([5])

        for name in self.fake.keys:
            down, up = await self.click_key(name)
            expected = self.fake.keys.index(name)
            self.assertEqual((down.message_type, down.payload),
                             (web_ui.KEY, bytes((expected, 1))))
            self.assertEqual((up.message_type, up.payload),
                             (web_ui.KEY_RELEASE_AFTER_SAMPLE, bytes((expected,))))

        # Pointer capture is normally enough to deliver pointer-up back to the
        # key. Keep a window-level fallback for browsers/platforms where
        # capture is unavailable and the pointer is released elsewhere.
        down_key = self.page.locator('[data-key="down"]')
        await down_key.evaluate("element => { element.setPointerCapture = () => {}; }")
        box = await down_key.bounding_box()
        self.assertIsNotNone(box)
        await self.page.mouse.move(box["x"] + box["width"] / 2,
                                   box["y"] + box["height"] / 2)
        await self.page.mouse.down()
        down = await self.next_control()
        await self.page.mouse.move(1, 1)
        await self.page.mouse.up()
        up = await self.next_control()
        down_index = self.fake.keys.index("down")
        self.assertEqual((down.message_type, down.payload),
                         (web_ui.KEY, bytes((down_index, 1))))
        self.assertEqual((up.message_type, up.payload),
                         (web_ui.KEY_RELEASE_AFTER_SAMPLE, bytes((down_index,))))

        await self.page.keyboard.down("1")
        down = await self.next_control()
        await self.page.keyboard.up("1")
        up = await self.next_control()
        self.assertEqual(down.payload, bytes((self.fake.keys.index("1"), 1)))
        self.assertEqual(up.message_type, web_ui.KEY_RELEASE_AFTER_SAMPLE)
        self.assertEqual(up.payload, bytes((self.fake.keys.index("1"),)))

        for keyboard_key, name in (("ArrowLeft", "left"), ("ArrowRight", "right"),
                                   ("q", "soft-left"), ("e", "soft-right")):
            await self.page.keyboard.down(keyboard_key)
            down = await self.next_control()
            await self.page.keyboard.up(keyboard_key)
            up = await self.next_control()
            self.assertEqual(down.payload, bytes((self.fake.keys.index(name), 1)))
            self.assertEqual(up.message_type, web_ui.KEY_RELEASE_AFTER_SAMPLE)
            self.assertEqual(up.payload, bytes((self.fake.keys.index(name),)))

        self.assertEqual(await self.page.title(), "x55 Emulator")
        self.assertEqual(await self.page.locator("#phone-model").text_content(), "M55")
        self.assertEqual(
            await self.page.locator(".phone").get_attribute("aria-label"),
            "M55 phone controls",
        )
        self.assertEqual(await self.page.locator(".product").count(), 0)
        self.assertEqual(await self.page.locator("#run-toggle").count(), 0)
        self.assertEqual(await self.page.locator("#icount").count(), 0)
        self.assertEqual(
            await self.page.locator(".runtime-metric dt").all_text_contents(),
            ["STATUS", "TICKS", "TICK RATE · STALE", "GUEST INSTR/S", "PC"],
        )
        self.assertEqual(
            await self.page.locator("#connection-state").get_attribute("class"),
            "connection connected",
        )
        self.assertEqual(
            await self.page.locator("#connection-label").get_attribute("class"),
            "visually-hidden",
        )
        self.assertEqual(
            await self.page.locator("#connection-label").text_content(),
            "Connected",
        )
        expected_metrics = {
            "#runtime-running": "61.14s",
            "#runtime-ticks": "42.5M",
            "#runtime-tick-rate": "--",
            "#runtime-guest-rate": "--",
            "#runtime-pc": "0xef4b98",
        }
        for selector, expected in expected_metrics.items():
            self.assertEqual(await self.page.locator(selector).text_content(), expected)

        shots = CEMU_ROOT / "shots" / "keypad-matrix" / "m55"
        shots.mkdir(parents=True, exist_ok=True)
        await self.page.screenshot(path=shots / "desktop.png", full_page=True)
        await self.page.set_viewport_size({"width": 390, "height": 844})
        await self.assert_layout([1, 2, 2])
        await self.page.screenshot(path=shots / "mobile.png", full_page=True)
        await self.page.set_viewport_size({"width": 280, "height": 700})
        await self.assert_layout([1, 2, 2])
        self.assertEqual(self.console_errors, [])

    async def test_asc0_actions_and_responsive_layout(self):
        shots = CEMU_ROOT / "shots" / "serial-console"
        shots.mkdir(parents=True, exist_ok=True)
        panel = self.page.locator("#asc0-panel")
        self.assertTrue(await panel.is_visible())
        await self.page.screenshot(path=shots / "empty-desktop.png", full_page=True)

        raw = b"<script>window.injected=true</script>\x00EXIT: B102 08 0095\r"
        await self.fake.send_asc0_tx(raw)
        expected = "<script>window.injected=true</script>\\x00\nEXIT: B102 08 0095\\x0D\n"
        await self.page.wait_for_function(
            "expected => document.querySelector('#asc0-output').textContent === expected",
            arg=expected)
        self.assertEqual(await self.page.locator("#asc0-output").text_content(), expected)
        self.assertIsNone(await self.page.evaluate("window.injected"))
        self.assertEqual(await panel.locator("script").count(), 0)
        self.assertFalse(await self.page.locator("#asc0-copy").is_disabled())
        await self.page.locator("#asc0-copy").click()
        self.assertEqual(await self.page.evaluate("window.__copiedConsole"), expected)

        async def assert_console_layout():
            layout = await self.page.evaluate("""() => {
              const box = (selector) => {
                const r = document.querySelector(selector).getBoundingClientRect();
                return {left: r.left, top: r.top, right: r.right, bottom: r.bottom,
                        width: r.width, height: r.height};
              };
              const output = document.querySelector('#asc0-output');
              const body = document.body;
              const title = document.querySelector('.asc0-title');
              const titleLabel = title.querySelector('span');
              const actions = document.querySelector('.asc0-actions');
              const barStyle = getComputedStyle(document.querySelector('.app-bar'));
              const titleStyle = getComputedStyle(title);
              const outputStyle = getComputedStyle(output);
              const panelStyle = getComputedStyle(document.querySelector('#asc0-panel'));
              return {phone: box('.phone'), panel: box('#asc0-panel'),
                      title: box('.asc0-title'), titleLabel: box('.asc0-title span'),
                      actions: box('.asc0-actions'),
                      actionButtons: [...actions.querySelectorAll('button')].map(button => {
                        const r = button.getBoundingClientRect();
                        return {left: r.left, top: r.top, right: r.right, bottom: r.bottom};
                      }),
                      output: {clientWidth: output.clientWidth,
                               scrollWidth: output.scrollWidth,
                               clientHeight: output.clientHeight,
                               scrollHeight: output.scrollHeight},
                      bar: box('.app-bar'),
                      barPadding: [barStyle.paddingTop, barStyle.paddingRight,
                                   barStyle.paddingBottom, barStyle.paddingLeft],
                      panelPadding: [panelStyle.paddingTop, panelStyle.paddingRight,
                                     panelStyle.paddingBottom, panelStyle.paddingLeft],
                      barBackground: barStyle.backgroundColor,
                      panelBackground: panelStyle.backgroundColor,
                      interior: {titleBackground: titleStyle.backgroundColor,
                                 titleBorderBottom: parseFloat(titleStyle.borderBottomWidth),
                                 titleBorderLeft: parseFloat(titleStyle.borderLeftWidth),
                                 titleBorderRight: parseFloat(titleStyle.borderRightWidth),
                                 outputBackground: outputStyle.backgroundColor,
                                 outputBorderTop: parseFloat(outputStyle.borderTopWidth),
                                 outputBorderLeft: parseFloat(outputStyle.borderLeftWidth),
                                 outputBorderRight: parseFloat(outputStyle.borderRightWidth)},
                      bodyVisible: body.classList.contains('asc0-visible'),
                      documentWidth: document.documentElement.scrollWidth,
                      viewportWidth: innerWidth,
                      viewportHeight: innerHeight,
                      panelPosition: panelStyle.position};
            }""")
            self.assertTrue(layout["bodyVisible"])
            self.assertEqual(layout["panelPosition"], "fixed")
            self.assertEqual(layout["panelBackground"], layout["barBackground"])
            self.assertEqual(layout["panelPadding"], layout["barPadding"])
            self.assertEqual(layout["interior"]["titleBackground"], "rgba(0, 0, 0, 0)")
            self.assertGreaterEqual(layout["interior"]["titleBorderBottom"], 1)
            self.assertEqual(layout["interior"]["titleBorderLeft"], 0)
            self.assertEqual(layout["interior"]["titleBorderRight"], 0)
            self.assertEqual(layout["interior"]["outputBackground"], "rgba(0, 0, 0, 0)")
            self.assertEqual(layout["interior"]["outputBorderTop"], 0)
            self.assertEqual(layout["interior"]["outputBorderLeft"], 0)
            self.assertEqual(layout["interior"]["outputBorderRight"], 0)
            self.assertGreater(layout["panel"]["width"], 0)
            self.assertGreater(layout["panel"]["height"], 0)
            self.assertAlmostEqual(layout["panel"]["left"], 0, delta=1)
            self.assertAlmostEqual(
                layout["panel"]["right"], layout["viewportWidth"], delta=1)
            self.assertAlmostEqual(
                layout["panel"]["bottom"], layout["viewportHeight"], delta=1)
            self.assertLess(layout["titleLabel"]["right"], layout["actions"]["left"])
            self.assertAlmostEqual(
                layout["actions"]["right"], layout["title"]["right"], delta=1)
            for button in layout["actionButtons"]:
                self.assertGreaterEqual(button["top"], layout["title"]["top"])
                self.assertLessEqual(button["bottom"], layout["title"]["bottom"])
            self.assertLessEqual(layout["bar"]["bottom"], layout["panel"]["top"])
            self.assertLessEqual(layout["phone"]["bottom"], layout["panel"]["top"])
            self.assertLessEqual(layout["documentWidth"], layout["viewportWidth"])
            self.assertLessEqual(layout["output"]["scrollWidth"],
                                 layout["output"]["clientWidth"])

        await assert_console_layout()
        await self.page.screenshot(path=shots / "visible-desktop.png", full_page=True)

        expanded_height = (await panel.bounding_box())["height"]
        self.assertAlmostEqual(expanded_height, 154, delta=1)
        await self.page.locator("#asc0-minimize").click()
        self.assertTrue(await self.page.locator("body").evaluate(
            "body => body.classList.contains('asc0-minimized')"
        ))
        self.assertEqual(
            await self.page.locator("#asc0-minimize").get_attribute("aria-expanded"),
            "false",
        )
        self.assertEqual(
            await self.page.locator("#asc0-minimize").get_attribute("title"),
            "Restore ASC0 TX",
        )
        self.assertTrue(await self.page.locator("#asc0-output").is_hidden())
        minimized_height = (await panel.bounding_box())["height"]
        self.assertAlmostEqual(minimized_height, 50, delta=1)
        self.assertLess(minimized_height, expanded_height)
        self.assertEqual(await self.page.locator("#asc0-output").text_content(), expected)
        await self.page.screenshot(path=shots / "minimized-desktop.png", full_page=True)

        await self.page.locator("#asc0-minimize").click()
        self.assertFalse(await self.page.locator("body").evaluate(
            "body => body.classList.contains('asc0-minimized')"
        ))
        self.assertEqual(
            await self.page.locator("#asc0-minimize").get_attribute("aria-expanded"),
            "true",
        )
        self.assertTrue(await self.page.locator("#asc0-output").is_visible())
        self.assertEqual(await self.page.locator("#asc0-output").text_content(), expected)
        await assert_console_layout()

        await self.page.set_viewport_size({"width": 390, "height": 844})
        await assert_console_layout()
        self.assertAlmostEqual((await panel.bounding_box())["height"], 100, delta=1)
        await self.page.locator("#asc0-minimize").click()
        self.assertAlmostEqual((await panel.bounding_box())["height"], 48, delta=1)
        await self.page.locator("#asc0-minimize").click()
        await assert_console_layout()
        await self.page.screenshot(path=shots / "visible-mobile.png", full_page=True)
        await self.page.evaluate("scrollTo(0, document.documentElement.scrollHeight)")
        await assert_console_layout()
        self.assertEqual(self.console_errors, [])

    async def test_asc0_selection_and_scroll_survive_live_updates(self):
        history = b"".join(
            f"line {index:03d} payload\r".encode() for index in range(80)
        )
        await self.fake.send_asc0_tx(history)
        output = self.page.locator("#asc0-output")
        await self.page.locator("#asc0-output", has_text="line 079 payload").wait_for()

        before = await output.evaluate("""element => {
          element.scrollTop = Math.floor(element.scrollHeight / 3);
          const node = element.firstChild;
          const range = document.createRange();
          range.setStart(node, 5);
          range.setEnd(node, 25);
          const selection = getSelection();
          selection.removeAllRanges();
          selection.addRange(range);
          return {scrollTop: element.scrollTop, selected: selection.toString(),
                  childNodes: element.childNodes.length, nodeType: node.nodeType};
        }""")

        await self.fake.send_asc0_tx(b"live one\rlive two\r")
        stats = web_ui.STATS_PAYLOAD.pack(62_000_000_000, 43_000_000,
                                          43_000_000, 0xEF4B9A, 1, 100, 0, 0.0, 0.0, 0)
        self.fake.writer.write(web_ui.encode_packet(
            web_ui.STATS, stats, sequence=101, icount=43_000_000
        ))
        await self.fake.writer.drain()
        await self.page.locator("#asc0-output", has_text="live two").wait_for()

        after = await output.evaluate("""element => ({
          scrollTop: element.scrollTop,
          selected: getSelection().toString(),
          childNodes: element.childNodes.length,
          nodeType: element.firstChild.nodeType,
        })""")
        self.assertEqual(after["selected"], before["selected"])
        self.assertEqual(after["scrollTop"], before["scrollTop"])
        self.assertEqual(after["childNodes"], 1)
        self.assertEqual(after["nodeType"], 3)

        await output.evaluate("""element => {
          getSelection().removeAllRanges();
          element.scrollTop = element.scrollHeight;
        }""")
        await self.fake.send_asc0_tx(b"follow tail\r")
        await self.page.locator("#asc0-output", has_text="follow tail").wait_for()
        self.assertTrue(await output.evaluate(
            "element => element.scrollHeight - element.scrollTop <= "
            "element.clientHeight + 2"
        ))
        self.assertEqual(self.console_errors, [])

    async def test_file_panel_and_mobile_tabs(self):
        self.assertTrue(await self.page.locator("#files-connect").is_enabled())
        self.assertTrue(await self.page.locator("#files-refresh").is_disabled())
        self.assertEqual(await self.page.locator("#files-status").text_content(),
                         "Not connected")

        async def assert_workspace(phone_visible, files_visible, tabs_visible,
                                   sticky_top=None, dock_height=154, centered=True):
            layout = await self.page.evaluate("""() => {
              const bounds = (selector) => {
                const element = document.querySelector(selector);
                const r = element.getBoundingClientRect();
                return {left: r.left, top: r.top, right: r.right, bottom: r.bottom,
                        width: r.width, height: r.height,
                        visible: getComputedStyle(element).display !== 'none'};
              };
              const tabs = document.querySelector('.mobile-tabs');
              const tabsVisible = getComputedStyle(tabs).display !== 'none';
              return {phone: bounds('.device-stack'), phoneSurface: bounds('.phone'),
                      files: bounds('#files-panel'), bar: bounds('.app-bar'),
                      tabs: bounds('.mobile-tabs'), dock: bounds('#asc0-panel'),
                      tabsTop: getComputedStyle(tabs).top,
                      topBoundary: tabsVisible ? bounds('.mobile-tabs').bottom :
                        bounds('.app-bar').bottom,
                      selected: document.querySelector('.workspace').dataset.mobileView,
                      documentWidth: document.documentElement.scrollWidth,
                      viewportWidth: innerWidth, viewportHeight: innerHeight,
                      scrollHeight: document.documentElement.scrollHeight};
            }""")
            self.assertEqual(layout["phone"]["visible"], phone_visible)
            self.assertEqual(layout["files"]["visible"], files_visible)
            self.assertEqual(layout["tabs"]["visible"], tabs_visible)
            self.assertLessEqual(layout["documentWidth"], layout["viewportWidth"])
            self.assertAlmostEqual(layout["dock"]["height"], dock_height, delta=1)
            if sticky_top is not None:
                self.assertEqual(layout["tabsTop"], f"{sticky_top}px")
            for name in ("phone", "files"):
                surface = layout[name]
                if not surface["visible"]:
                    continue
                self.assertGreaterEqual(surface["left"], 0)
                self.assertLessEqual(surface["right"], layout["viewportWidth"])
            if files_visible:
                self.assertLessEqual(layout["files"]["width"], 520)
                self.assertLessEqual(layout["files"]["bottom"], layout["dock"]["top"])
            if files_visible and not phone_visible:
                self.assertAlmostEqual(layout["files"]["left"],
                                       layout["viewportWidth"] - layout["files"]["right"],
                                       delta=1)
            if phone_visible and not files_visible:
                self.assertAlmostEqual(layout["phone"]["left"] -
                                       (52 if layout["viewportWidth"] <= 480 else 42),
                                       layout["viewportWidth"] - layout["phone"]["right"],
                                       delta=1)
            if centered:
                visible_surfaces = []
                if phone_visible:
                    visible_surfaces.append(layout["phoneSurface"])
                if files_visible:
                    visible_surfaces.append(layout["files"])
                for surface in visible_surfaces:
                    top_gap = surface["top"] - layout["topBoundary"]
                    bottom_gap = layout["dock"]["top"] - surface["bottom"]
                    self.assertGreaterEqual(top_gap, 0)
                    self.assertGreaterEqual(bottom_gap, 0)
                    self.assertAlmostEqual(top_gap, bottom_gap, delta=2)
            return layout

        # The two-column layout remains active immediately above the new
        # breakpoint, and both surfaces fit without exposing the tabs.
        await self.page.set_viewport_size({"width": 801, "height": 900})
        await assert_workspace(True, True, False)
        await self.assert_layout([5])
        await self.page.locator("#asc0-minimize").click()
        await assert_workspace(True, True, False, dock_height=50)
        await self.page.locator("#asc0-minimize").click()

        # Desktop-status narrow layouts use one full-width selected surface.
        await self.page.set_viewport_size({"width": 800, "height": 900})
        layout = await assert_workspace(True, False, True, sticky_top=64)
        self.assertEqual(layout["selected"], "phone")
        await self.assert_layout([5])
        await self.page.locator("#tab-files").click()
        layout = await assert_workspace(False, True, True, sticky_top=64)
        self.assertEqual(layout["selected"], "files")
        self.assertEqual(await self.page.locator("#tab-files").get_attribute("aria-selected"),
                         "true")

        # Resizing does not alter the tab selection or filesystem state. Wide
        # mode merely makes both surfaces visible again.
        files_status = await self.page.locator("#files-status").text_content()
        await self.page.set_viewport_size({"width": 801, "height": 900})
        layout = await assert_workspace(True, True, False)
        self.assertEqual(layout["selected"], "files")
        await self.page.set_viewport_size({"width": 640, "height": 900})
        layout = await assert_workspace(False, True, True, sticky_top=64)
        self.assertEqual(layout["selected"], "files")
        self.assertEqual(await self.page.locator("#files-status").text_content(), files_status)

        # At the compact breakpoint the selected files surface still clears
        # the smaller dock, and the phone/files controls continue to switch.
        await self.page.set_viewport_size({"width": 480, "height": 844})
        layout = await assert_workspace(False, True, True, sticky_top=130,
                                        dock_height=100)
        self.assertEqual(layout["selected"], "files")
        await self.page.locator("#tab-phone").click()
        layout = await assert_workspace(True, False, True, sticky_top=130,
                                        dock_height=100)
        self.assertEqual(layout["selected"], "phone")
        await self.assert_layout([1, 2, 2])

        # If the phone cannot fit at its existing scale, the document scrolls
        # far enough to expose its bottom edge above the fixed console.
        await self.page.set_viewport_size({"width": 280, "height": 700})
        layout = await assert_workspace(True, False, True, sticky_top=130,
                                        dock_height=100, centered=False)
        self.assertGreater(layout["scrollHeight"], layout["viewportHeight"])
        await self.page.evaluate("scrollTo(0, document.documentElement.scrollHeight)")
        scrolled = await self.page.evaluate("""() => {
          const phone = document.querySelector('.phone').getBoundingClientRect();
          const dock = document.querySelector('#asc0-panel').getBoundingClientRect();
          return {phoneBottom: phone.bottom, dockTop: dock.top,
                  documentWidth: document.documentElement.scrollWidth,
                  viewportWidth: innerWidth};
        }""")
        self.assertLessEqual(scrolled["phoneBottom"], scrolled["dockTop"])
        self.assertLessEqual(scrolled["documentWidth"], scrolled["viewportWidth"])
        self.assertEqual(self.console_errors, [])

    async def test_reconnect_state(self):
        await self.page.keyboard.down("q")
        self.assertEqual((await self.next_control()).message_type, web_ui.KEY)
        await self.fake.stop()
        await self.page.locator("#connection-state.error").wait_for()
        self.assertEqual(
            await self.page.locator("#connection-state").get_attribute("class"),
            "connection error",
        )
        self.assertEqual(
            await self.page.locator("#connection-label").text_content(),
            "Disconnected",
        )
        # A release while native transport is absent must update bridge ownership.
        await self.page.keyboard.up("q")
        await wait_until(lambda: not any("soft-left" in c.held for c in self.bridge.clients))
        self.fake = FakeCemu(self.path, keys=C55_BUTTONS)
        await self.fake.start()
        await wait_until(lambda: self.bridge.connection_count >= 2)
        await self.page.locator("#connection-state.connected").wait_for()
        await self.page.locator("#phone-model", has_text="C55").wait_for()
        visible = await self.page.locator("[data-key]:not([hidden])").evaluate_all(
            "elements => elements.map(element => element.dataset.key)"
        )
        self.assertEqual(len(visible), len(C55_BUTTONS))
        self.assertEqual(set(visible), set(C55_BUTTONS))
        self.assertTrue(await self.page.locator('[data-key="left"]').is_hidden())
        self.assertTrue(await self.page.locator('[data-key="right"]').is_hidden())
        self.assertFalse(await self.page.locator('[data-key="soft-left"]').is_hidden())
        await self.page.keyboard.down("ArrowLeft")
        await self.page.keyboard.up("ArrowLeft")
        with self.assertRaises(TimeoutError):
            await asyncio.wait_for(self.fake.received.get(), 0.1)
        await self.page.keyboard.down("q")
        down = await self.next_control()
        await self.page.keyboard.up("q")
        up = await self.next_control()
        index = C55_BUTTONS.index("soft-left")
        self.assertEqual(down.payload, bytes((index, 1)))
        self.assertEqual(up.message_type, web_ui.KEY_RELEASE_AFTER_SAMPLE)
        self.assertEqual(up.payload, bytes((index,)))
        self.assertEqual(
            await self.page.locator("#connection-state").get_attribute("class"),
            "connection connected",
        )
        await self.assert_layout([5])
        shots = CEMU_ROOT / "shots" / "keypad-matrix" / "c55"
        shots.mkdir(parents=True, exist_ok=True)
        await self.page.screenshot(path=shots / "desktop.png", full_page=True)
        await self.page.set_viewport_size({"width": 390, "height": 844})
        await self.assert_layout([1, 2, 2])
        await self.page.screenshot(path=shots / "mobile.png", full_page=True)
        self.assertEqual(await self.page.locator("#run-toggle").count(), 0)

    async def test_capability_zero_hides_files_and_asc0(self):
        await self.fake.stop()
        await self.page.locator("#connection-state.error").wait_for()
        self.fake = FakeCemu(self.path, model="c55", width=101, height=64,
                             keys=C55_BUTTONS, asc0_available=False)
        await self.fake.start()
        await wait_until(lambda: self.bridge.connection_count >= 2)
        await self.page.locator("#connection-state.connected").wait_for()
        await self.page.locator("#phone-model", has_text="C55").wait_for()

        self.assertTrue(await self.page.locator("#tab-files").is_hidden())
        self.assertTrue(await self.page.locator(".mobile-tabs").is_hidden())
        self.assertTrue(await self.page.locator("#files-panel").is_hidden())
        self.assertTrue(await self.page.locator("#asc0-panel").is_hidden())
        self.assertTrue(await self.page.locator(".device-stack").is_visible())
        self.assertEqual(
            await self.page.locator(".workspace").get_attribute("data-mobile-view"),
            "phone",
        )

        down, up = await self.click_key("soft-left")
        index = C55_BUTTONS.index("soft-left")
        self.assertEqual((down.message_type, down.payload),
                         (web_ui.KEY, bytes((index, 1))))
        self.assertEqual((up.message_type, up.payload),
                         (web_ui.KEY_RELEASE_AFTER_SAMPLE, bytes((index,))))
        pixels = await self.page.locator("#lcd").evaluate("""canvas =>
          Array.from(canvas.getContext('2d').getImageData(0, 0, 1, 1).data)
        """)
        self.assertEqual(pixels, [12, 34, 56, 255])

        await self.page.set_viewport_size({"width": 390, "height": 844})
        self.assertTrue(await self.page.locator(".device-stack").is_visible())
        self.assertTrue(await self.page.locator(".mobile-tabs").is_hidden())
        self.assertEqual(self.console_errors, [])


    async def connect_memory_files(self):
        from tools.obex.tests.fakes import MemoryClient
        phone = MemoryClient()
        self.web.files.client_factory = lambda: phone
        await self.page.locator("#files-connect").click()
        await self.page.get_by_text("This folder is empty", exact=True).wait_for()
        return phone

    async def test_files_malformed_reply_updates_status_without_relisting(self):
        from tools.obex import codec
        from tools.obex.client import ObexFilesystemClient
        phone = await self.connect_memory_files()
        original_list, original_get = phone._list, phone._get
        original_write = phone.transport.write_all
        phone._list = ObexFilesystemClient._list.__get__(phone)
        phone._get = ObexFilesystemClient._get.__get__(phone)
        requests = []
        self.page.on("request", lambda request: requests.append(request.url))

        async def malformed_packet(data, phase):
            if phase == "obex-get":
                phone.transport.incoming.extend(bytes((codec.SUCCESS, 0, 2)))
            else:
                await original_write(data, phase)
        phone.transport.write_all = malformed_packet
        await self.page.locator("#files-refresh").click()
        await self.page.wait_for_function("() => document.querySelector('#files-connect').textContent === 'Retry'")
        self.assertEqual(phone.state, "error")
        self.assertFalse(phone.transport.opened)
        self.assertNotEqual(await self.page.locator("#files-status").inner_text(), "Connected")
        for control in ("refresh", "upload", "mkdir"):
            self.assertTrue(await self.page.locator(f"#files-{control}").is_disabled())
        self.assertIn("length", await self.page.locator("#files-error").inner_text())
        self.assertEqual(sum('/api/files/list?' in url for url in requests), 1)
        self.assertEqual(sum('/api/files/status' in url for url in requests), 1)
        phone._list, phone._get = original_list, original_get
        phone.transport.write_all = original_write
        await self.page.locator("#files-connect").click()
        await self.page.wait_for_function("() => document.querySelector('#files-status').textContent === 'Connected'")
        self.assertTrue(await self.page.locator("#files-refresh").is_enabled())

    async def test_files_remote_error_preserves_healthy_session(self):
        from tools.obex.errors import RemoteError
        phone = await self.connect_memory_files()
        original_list = phone._list
        calls = 0
        async def denied(path):
            nonlocal calls
            calls += 1
            raise RemoteError(0xc3, "folder access denied")
        phone._list = denied
        await self.page.locator("#files-refresh").click()
        await self.page.get_by_text("folder access denied", exact=True).wait_for()
        self.assertEqual(calls, 1)
        self.assertEqual(phone.state, "ready")
        self.assertTrue(phone.transport.opened)
        self.assertEqual(await self.page.locator("#files-status").inner_text(), "Connected")
        self.assertEqual(await self.page.locator("#files-connect").inner_text(), "Connect")
        self.assertTrue(await self.page.locator("#files-refresh").is_enabled())
        phone._list = original_list
        await self.page.locator("#files-refresh").click()
        await self.page.locator("#files-error").wait_for(state="hidden")

    async def test_files_upload_conflicts_accept_json_and_text_errors(self):
        phone = await self.connect_memory_files()
        for content_type, body, message in (
            ("application/json", json.dumps({"error": "path_conflict", "message": "target is a folder"}), "target is a folder"),
            ("text/plain", "transfer identifier already used\n", "transfer identifier already used"),
        ):
            with self.subTest(content_type=content_type):
                async def conflict(route):
                    await route.fulfill(status=409, content_type=content_type, body=body)
                requests = []
                def record(request):
                    requests.append(request.url)
                self.page.on("request", record)
                await self.page.route('**/api/files/upload?*', conflict)
                await self.page.locator("#files-upload-input").set_input_files({
                    "name": "conflict.bin", "mimeType": "application/octet-stream", "buffer": b"new"})
                await self.page.get_by_text(message, exact=True).wait_for()
                self.assertEqual(phone.state, "ready")
                self.assertTrue(await self.page.locator("#files-refresh").is_enabled())
                self.assertFalse(any('/api/files/list?' in url for url in requests))
                self.assertEqual(sum('/api/files/status' in url for url in requests), 1)
                await self.page.unroute('**/api/files/upload?*', conflict)
                self.page.remove_listener("request", record)

    async def test_files_stream_replacement_progress_and_cancel(self):
        from tools.obex.tests.fakes import MemoryClient
        phone = MemoryClient()
        self.web.files.client_factory = lambda: phone
        await self.page.locator("#files-connect").click()
        await self.page.wait_for_function("() => document.querySelector('#files-status').textContent === 'Connected'")
        data = bytes(range(256))
        await self.page.locator("#files-upload-input").set_input_files({
            "name": "roundtrip.bin", "mimeType": "application/octet-stream", "buffer": data})
        await self.page.get_by_text("roundtrip.bin", exact=True).wait_for()
        async with self.page.expect_download() as pending:
            await self.page.get_by_role("link", name="Download", exact=True).click()
        download = await pending.value
        self.assertEqual(Path(await download.path()).read_bytes(), data)
        self.page.on("dialog", lambda dialog: dialog.accept())
        await self.page.locator("#files-upload-input").set_input_files({
            "name": "roundtrip.bin", "mimeType": "application/octet-stream", "buffer": b"replacement"})
        await wait_until(lambda: phone.files["A:\\roundtrip.bin"].size == 11)
        entered = asyncio.Event()
        async def blocked(path, source, *, progress=None):
            from tools.obex.models import TransferProgress
            await progress(TransferProgress("upload", path, 3, 100, "running"))
            entered.set()
            await asyncio.Future()
        phone._upload = blocked
        await self.page.locator("#files-upload-input").set_input_files({
            "name": "cancel.bin", "mimeType": "application/octet-stream", "buffer": b"cancel me"})
        await asyncio.wait_for(entered.wait(), 3)
        cancel = self.page.get_by_role("button", name="Cancel", exact=True)
        await self.page.wait_for_function("() => Array.from(document.querySelectorAll('button')).some(b => b.textContent === 'Cancel' && !b.disabled)")
        self.assertIn("3 B / 100 B", await self.page.locator("#files-panel").inner_text())
        await cancel.click()
        await self.page.wait_for_function("() => document.querySelector('#files-connect').textContent === 'Retry'")
        self.assertEqual(phone.state, "error")
        self.assertNotIn("A:\\cancel.bin", phone.files)

class BrowserStartupTests(unittest.IsolatedAsyncioTestCase):
    """Fault only the first browser socket; replacements use the real bridge."""

    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cemu-browser-startup-")
        self.addCleanup(self.temp.cleanup)
        self.path = os.path.join(self.temp.name, "cemu.sock")
        self.fake = FakeCemu(self.path)
        self.addAsyncCleanup(self.fake.stop)
        self.bridge = web_ui.CemuBridge(self.path, reconnect_delay=0.03)
        self.bridge_task = asyncio.create_task(self.bridge.run())
        self.addAsyncCleanup(self.stop_bridge)
        self.web = web_ui.WebServer(self.bridge, "127.0.0.1", 0)
        await self.web.start()
        self.addAsyncCleanup(self.web.stop)
        self.playwright = await async_playwright().start()
        self.addAsyncCleanup(self.playwright.stop)
        self.browser = await launch_browser(self.playwright)
        self.addAsyncCleanup(self.browser.close)
        self.page = await self.browser.new_page()
        self.errors = []
        self.page.on("pageerror", lambda error: self.errors.append(str(error)))

    async def stop_bridge(self):
        await self.bridge.stop()
        self.bridge_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await self.bridge_task

    async def load(self, fault="healthy", suspend_timer=False):
        await self.page.add_init_script("""(() => {
          const NativeWebSocket = window.WebSocket;
          window.__attempts = [];
          window.__connectionTiming = [];
          window.__scheduledDelays = [];
          const now = Date.now.bind(Date);
          window.__wallOffset = 0;
          Date.now = () => now() + window.__wallOffset;
          const schedule = window.setTimeout.bind(window);
          window.setTimeout = (callback, delay, ...args) => {
            if (delay === 5000 || delay === 500) window.__scheduledDelays.push(delay);
            if (SUSPEND_TIMER && delay === 5000) {
              // Simulate a background tab whose startup callback is suspended.
              return schedule(callback, 60000, ...args);
            }
            return schedule(callback, delay, ...args);
          };
          class SilentSocket extends EventTarget {
            constructor() {
              super();
              this.readyState = FAULT === 'connecting' ? 0 : 1;
              this.closeCalls = 0;
              this.sent = [];
              if (this.readyState === 1) queueMicrotask(() => this.emit('open'));
            }
            emit(type, data) {
              this.dispatchEvent(type === 'message' ?
                new MessageEvent(type, {data}) : new Event(type));
            }
            send(data) { this.sent.push(data); }
            close() {
              this.closeCalls++;
              window.__connectionTiming.push({event: 'retire', time: performance.now()});
            } // Deliberately never emits close.
          }
          window.WebSocket = class {
            static OPEN = NativeWebSocket.OPEN;
            constructor(url) {
              window.__connectionTiming.push({event: 'construct', time: performance.now()});
              const socket = FAULT !== 'healthy' && !window.__attempts.length ?
                new SilentSocket() : new NativeWebSocket(url);
              window.__attempts.push(socket);
              return socket;
            }
          };
        })()""".replace("FAULT", json.dumps(fault)).replace(
            "SUSPEND_TIMER", json.dumps(suspend_timer)))
        await self.page.goto(f"http://127.0.0.1:{self.web.port}/")

    async def start_emulator(self):
        await self.fake.start()
        await asyncio.wait_for(self.fake.connected.wait(), 2)
        await wait_until(lambda: self.bridge.frame is not None)

    async def assert_snapshot(self):
        await self.page.wait_for_function("""() =>
          document.querySelector('#connection-state').classList.contains('connected') &&
          document.querySelector('#phone-model').textContent === 'C55' &&
          document.querySelectorAll('[data-key]:not([hidden])').length === 18 &&
          document.querySelector('#runtime-pc').textContent === '0xef4b98' &&
          document.querySelector('#lcd').getContext('2d').getImageData(0, 0, 1, 1).data[0] === 12
        """, timeout=8000)
        snapshot_time = asyncio.get_running_loop().time()
        self.assertEqual(await self.page.locator('[data-key]:not([hidden])').evaluate_all(
            "elements => elements.map(element => element.dataset.key).sort()"),
            sorted(C55_BUTTONS))
        self.assertEqual(await self.page.locator('#lcd').evaluate("""canvas =>
          [...canvas.getContext('2d').getImageData(0, 0, 1, 1).data]
        """), [12, 34, 56, 255])
        self.assertEqual(self.errors, [])
        return snapshot_time

    async def assert_advancing_stats(self):
        previous = await self.page.locator('#runtime-ticks').text_content()
        stats = web_ui.STATS_PAYLOAD.pack(62_000_000_000, 43_000_000,
                                          43_000_000, 0xEF4B98, 1, 100, 0, 0.0, 0.0, 0)
        self.fake.writer.write(web_ui.encode_packet(
            web_ui.STATS, stats, sequence=101, icount=43_000_000))
        await self.fake.writer.drain()
        await self.page.wait_for_function(
            "previous => document.querySelector('#runtime-ticks').textContent !== previous",
            arg=previous)

    async def assert_retired_events_ignored(self):
        await self.page.keyboard.down('1')
        await asyncio.wait_for(self.fake.received.get(), 2)
        await self.page.evaluate("""() => {
          const old = window.__attempts[0];
          const current = window.__attempts[1];
          window.__lateSends = [];
          const send = current.send.bind(current);
          current.send = data => { window.__lateSends.push(data); send(data); };
          old.emit('open');
          old.emit('message', JSON.stringify({type: 'state', bridge: 'disconnected',
            model: 'stale', keys: [], width: 1, height: 1}));
          old.emit('message', new Uint8Array([1, 255, 0, 0]).buffer);
          old.emit('message', JSON.stringify({type: 'asc0_tx', text: 'STALE'}));
          old.emit('error');
          old.emit('close');
        }""")
        await asyncio.sleep(0.7)
        await self.assert_snapshot()
        self.assertEqual(await self.page.evaluate('window.__attempts.length'), 2)
        self.assertEqual(await self.page.evaluate('window.__lateSends'), [])
        self.assertNotIn('STALE', await self.page.locator('#asc0-output').text_content())
        self.assertEqual(await self.page.evaluate('window.__attempts[0].closeCalls'), 1)
        await self.page.keyboard.up('1')

    async def recover_stalled_socket(self, fault):
        await self.load(fault)
        self.assertEqual(await self.page.locator('#connection-state').get_attribute('class'),
                         'connection connecting')
        self.assertEqual(await self.page.locator('[data-key]:not([hidden])').count(), 0)
        started = asyncio.get_running_loop().time()
        await self.start_emulator()
        snapshot_time = await self.assert_snapshot()
        elapsed = snapshot_time - started
        timing = await self.page.evaluate('window.__connectionTiming')
        first, retired, replacement = timing[:3]
        watchdog_ms = retired['time'] - first['time']
        retry_ms = replacement['time'] - retired['time']
        print(f"startup timing ({fault}): watchdog={watchdog_ms:.1f}ms retry={retry_ms:.1f}ms coherent={elapsed:.3f}s")
        self.assertEqual(await self.page.evaluate('window.__scheduledDelays.slice(0, 2)'), [5000, 500])
        self.assertGreaterEqual(watchdog_ms, 4900)
        self.assertGreaterEqual(retry_ms, 450)
        # Browser scheduling can deliver timers late; bound the complete recovery
        # separately from the exact requested watchdog and retry delays.
        self.assertLess(elapsed, 8)
        self.assertGreater(elapsed, 4.5)
        await self.assert_advancing_stats()
        await self.assert_retired_events_ignored()

    async def test_startup_stuck_connecting(self):
        await self.recover_stalled_socket('connecting')

    async def test_reconnect_replays_held_keys_and_pagehide_releases_before_close(self):
        await self.load('silent')
        await self.start_emulator()
        await self.page.evaluate("""state => {
          window.__attempts[0].emit('message', JSON.stringify(state));
        }""", self.bridge.status())
        await self.page.keyboard.down('1')
        self.assertEqual(await self.page.evaluate(
            "window.__attempts[0].sent.map(data => JSON.parse(data))"),
            [{"type": "key", "key": "1", "pressed": True, **self.bridge.identifiers()}])
        await self.page.evaluate("window.__attempts[0].emit('error')")
        await self.assert_snapshot()
        replay = await asyncio.wait_for(self.fake.received.get(), 2)
        self.assertEqual((replay.message_type, replay.payload),
                         (web_ui.KEY, bytes((C55_BUTTONS.index('1'), 1))))
        await self.page.evaluate("""() => {
          const socket = window.__attempts[1];
          const send = socket.send.bind(socket);
          const close = socket.close.bind(socket);
          window.__teardown = [];
          socket.send = data => { window.__teardown.push(JSON.parse(data)); send(data); };
          socket.close = () => { window.__teardown.push('close'); close(); };
          window.dispatchEvent(new Event('pagehide'));
        }""")
        self.assertEqual(await self.page.evaluate('window.__teardown'),
                         [{"type": "release_all", **self.bridge.identifiers()}, "close"])
        self.assertEqual(await self.page.locator('.pressed').count(), 0)
        await self.page.keyboard.up('1')
        self.assertEqual(self.errors, [])

    async def test_files_initialize_once_across_capability_changes(self):
        requests = []

        async def status(route):
            requests.append(route.request.url)
            await route.fulfill(json={"state": "disconnected"})

        await self.page.route('**/api/files/status', status)
        await self.load('silent')
        for available in (True, False, True):
            state = self.runtime_state(asc0_available=available)
            await self.emit(state)
            if available:
                await wait_until(lambda: len(requests) >= 1)
                await self.page.locator('#files-panel').wait_for(state='visible')
                await self.page.locator('#asc0-panel').wait_for(state='visible')
            else:
                await self.page.locator('#files-panel').wait_for(state='hidden')
                await self.page.locator('#asc0-panel').wait_for(state='hidden')
        await self.page.wait_for_load_state('networkidle')
        self.assertEqual(len(requests), 1)
        self.assertEqual(self.errors, [])

    async def test_statistics_consumers_coalesce_with_independent_clocks(self):
        await self.load('silent')
        result = await self.page.evaluate("""async () => {
          const {createStatus} = await import('/status.js');
          const original = {now: performance.now, set: window.setTimeout, clear: window.clearTimeout};
          let now = 0, next = 1;
          const timers = new Map();
          performance.now = () => now;
          window.setTimeout = (fn, delay) => { const id = next++; timers.set(id, {fn, at: now + delay}); return id; };
          window.clearTimeout = id => timers.delete(id);
          const step = time => {
            now = time;
            for (const [id, task] of [...timers]) if (task.at <= now && timers.delete(id)) task.fn();
          };
          const sample = (sequence, extra = {}) => ({sample_sequence: String(sequence),
            measured_ns: String(sequence * 10000000), elapsed_ns: String(sequence * 1000000000),
            ticks: String(sequence * 1000000), guest_instructions: String(sequence * 500000),
            pc: sequence, age_ns: '0', window_ns: '1000000000', rates_valid: true,
            ticks_per_s: sequence * 1000000, guest_instructions_per_s: 0, ...extra});
          const read = () => ['runtime-running', 'runtime-ticks', 'runtime-tick-rate',
                             'runtime-guest-rate', 'runtime-pc'].map(id => document.getElementById(id).textContent);
          const results = [];
          try {
            // Separate sinks can share publications without sharing timers.
            const fast = createStatus(50), slow = createStatus(100);
            fast.updateMetrics(sample(1)); slow.updateMetrics(sample(1));
            for (let i = 2; i <= 5; i++) {
              step((i - 1) * 10); fast.updateMetrics(sample(i)); slow.updateMetrics(sample(i));
            }
            results.push(read());
            step(50); results.push(read()); // fast consumes latest, slow still pending
            fast.clear(); // does not cancel the slow consumer
            step(100); results.push(read());
            slow.clear();
            const status = createStatus(50);
            status.updateMetrics(sample(1, {rates_valid: false})); results.push(read());
            for (let i = 2; i <= 1000; i++) status.updateMetrics(sample(i));
            results.push(timers.size); // one pending render and one freshness timeout
            step(1000); results.push(read()); // one render, no replay of 999 samples
            results.push(timers.size);
            status.updateMetrics(sample(1001, {age_ns: '1900000000'}));
            step(1250); results.push(document.getElementById('measurement-stale').hidden);
            status.updateMetrics(sample(1001)); // duplicate cannot renew age
            step(1500); results.push(document.getElementById('measurement-stale').hidden);
            status.updateMetrics(sample(1002, {elapsed_ns: '18446744073709551615',
              ticks: '18446744073709551615', pc: 0xabc, ticks_per_s: 0,
              guest_instructions_per_s: 1234567}));
            status.updateLifecycle({phase: 'stopped', status: 'limit'});
            results.push(read()); // final flush before next 50 ms deadline
            status.updateMetrics(sample(1003)); results.push(read()); // final STATS after lifecycle
            status.updateMetrics(sample(1004, {ticks_per_s: 1250000, guest_instructions_per_s: 1150000}));
            results.push(read());
            status.clear();
            status.updateMetrics(sample(1));
            status.updateMetrics(sample(2));
            status.clear(); // replacement must retire old pending timers
            status.updateState({bridge: 'connected', lifecycle: {phase: 'running'}, stats: sample(1)});
            step(1700); results.push(read());
            status.updateMetrics(sample(2));
            status.setConnectionState('error');
            step(4000); results.push(read());
            results.push(document.getElementById('measurement-stale').hidden);
            results.push(document.getElementById('connection-label').textContent);
            status.clear(); results.push(timers.size);
          } finally {
            performance.now = original.now; window.setTimeout = original.set; window.clearTimeout = original.clear;
          }
          return results;
        }""")
        def fields(n):
            return [f'{n:.2f}s', f'{n:.1f}M', f'{n:.1f}M/s', '0.0M/s', f'0x{n:06x}']
        self.assertEqual(result, [fields(1), fields(5), fields(5),
            ['1.00s', '1.0M', '--', '--', '0x000001'], 2, fields(1000), 1,
            False, False,
            ['18446744073.71s', '18446744073709.6M', '0.0M/s', '1.2M/s', '0x000abc'],
            fields(1003), ['1004.00s', '1004.0M', '1.3M/s', '1.1M/s', '0x0003ec'],
            fields(1), fields(2), False, 'Disconnected', 0])
        self.assertEqual(self.errors, [])

    async def test_measurement_freshness_warmup_delays_duplicates_and_replacement(self):
        await self.load('silent')
        state = self.runtime_state()
        stats = {**state['stats'], "sample_sequence": "1", "measured_ns": "100",
                 "age_ns": "0", "window_ns": "0", "rates_valid": False,
                 "ticks_per_s": None, "guest_instructions_per_s": None}
        await self.emit({**state, "stats": stats})
        self.assertEqual(await self.page.locator('#runtime-tick-rate').inner_text(), '--')
        self.assertFalse(await self.page.locator('#measurement-stale').is_visible())
        # Native and bridge queue age is already two seconds at receipt.
        stats = {**stats, "sample_sequence": "2", "measured_ns": "200",
                 "age_ns": "2000000000", "window_ns": "1234567890", "rates_valid": True,
                 "ticks_per_s": 1234567., "guest_instructions_per_s": 0.,
                 "ticks": "18446744073709551615"}
        await self.emit({**state, "type": "metrics", "stats": stats})
        await self.page.locator('#measurement-stale').wait_for(state='visible')
        self.assertEqual(await self.page.locator('#runtime-ticks').inner_text(), '18446744073709.6M')
        self.assertIn('1234567890 ns', await self.page.locator('#runtime-tick-rate').get_attribute('title'))
        self.assertEqual(await self.page.locator('#runtime-guest-rate').inner_text(), '0.0M/s')
        # Bootstrap, same-run reconnect, lifecycle and transport cannot freshen it.
        await self.emit({**state, "generation": 2, "stats": {**stats, "age_ns": "0"}})
        await self.emit({**state, "type": "transport", "generation": 2, "bridge": "disconnected"})
        await self.emit({**state, "type": "lifecycle", "generation": 2, "lifecycle": {"phase": "stopped"}})
        self.assertTrue(await self.page.locator('#measurement-stale').is_visible())
        self.assertEqual(await self.page.locator('#runtime-tick-rate').inner_text(), '1.2M/s')
        # New measurement restores freshness. Locally elapsed time ages it too.
        stats = {**stats, "sample_sequence": "3", "measured_ns": "300", "age_ns": "1900000000"}
        await self.emit({**state, "type": "metrics", "generation": 2, "stats": stats})
        self.assertFalse(await self.page.locator('#measurement-stale').is_visible())
        await self.page.locator('#measurement-stale').wait_for(state='visible')
        # Visibility restoration rechecks even if the timer was throttled.
        stats = {**stats, "sample_sequence": "4", "measured_ns": "400", "age_ns": "0"}
        await self.emit({**state, "type": "metrics", "generation": 2, "stats": stats})
        await self.page.evaluate("""() => {
            const original = performance.now.bind(performance);
            performance.now = () => original() + 2500;
            document.dispatchEvent(new Event('visibilitychange'));
            performance.now = original;
        }""")
        self.assertTrue(await self.page.locator('#measurement-stale').is_visible())
        await self.emit({**state, "run_id": "22" * 16, "generation": 3,
                         "stats": {**stats, "sample_sequence": "1", "measured_ns": "500",
                                   "rates_valid": False, "ticks": "0"}})
        self.assertFalse(await self.page.locator('#measurement-stale').is_visible())
        self.assertEqual(await self.page.locator('#runtime-tick-rate').inner_text(), '--')
        self.assertEqual(self.errors, [])

    def runtime_state(self, **changes):
        return {"type": "state", "bridge": "connected", "run_id": "11" * 16,
                "generation": 1, "model": "c55", "width": 101, "height": 64,
                "keys": list(C55_BUTTONS), "asc0_available": False, "audio_available": False,
                "asc0_tx": "old serial", "lifecycle": {"phase": "running", "status": "", "reason": ""},
                "stats": {"elapsed_ns": "1000000000", "ticks": "1000000", "guest_instructions": "900000", "pc": 0x123456}, **changes}

    async def emit(self, message):
        await self.page.evaluate("""message => window.__attempts[0].emit('message', JSON.stringify(message))""", message)

    async def test_run_replacement_clears_atomically_and_retired_generation_is_ignored(self):
        await self.load('silent')
        first = self.runtime_state()
        await self.emit(first)
        await self.page.evaluate("""() => {
          const canvas = document.querySelector('#lcd');
          const ctx = canvas.getContext('2d');
          ctx.fillStyle = '#ff0000'; ctx.fillRect(0, 0, canvas.width, canvas.height);
          window.__keyNode = document.querySelector('[data-key="1"]');
        }""")
        await self.emit({"type": "metrics", "run_id": first["run_id"], "generation": 1,
                         "stats": {**first["stats"], "ticks": "2000000"}, "keys": [], "width": 1, "asc0_available": True})
        await self.page.wait_for_function("() => document.getElementById('runtime-ticks').textContent === '2.0M'")
        self.assertTrue(await self.page.evaluate("window.__keyNode === document.querySelector('[data-key=\"1\"]')"))
        await self.emit({"type": "transport", "run_id": first["run_id"], "generation": 1, "bridge": "disconnected"})
        self.assertEqual(await self.page.locator('#runtime-ticks').inner_text(), '2.0M')
        self.assertEqual(await self.page.evaluate("Array.from(document.querySelector('#lcd').getContext('2d').getImageData(0,0,1,1).data)"), [255, 0, 0, 255])
        await self.emit(self.runtime_state(generation=2, stats={**first["stats"], "ticks": "2000000"}, asc0_tx=""))
        self.assertEqual(await self.page.evaluate("Array.from(document.querySelector('#lcd').getContext('2d').getImageData(0,0,1,1).data)"), [255, 0, 0, 255])
        second = self.runtime_state(run_id="22" * 16, generation=3, asc0_tx="", stats={**first["stats"], "ticks": "0", "elapsed_ns": "0"})
        await self.emit(second)
        self.assertEqual(await self.page.locator('#runtime-ticks').inner_text(), '0.0M')
        self.assertEqual(await self.page.evaluate("Array.from(document.querySelector('#lcd').getContext('2d').getImageData(0,0,1,1).data)"), [202, 213, 167, 255])
        await self.emit({"type": "metrics", "run_id": first["run_id"], "generation": 2, "stats": first["stats"]})
        await self.page.evaluate("""() => {
          const bytes = new Uint8Array(25 + 101 * 64 * 3).fill(255);
          bytes[0] = 1; bytes.fill(0x11, 1, 17);
          new DataView(bytes.buffer).setBigUint64(17, 2n, true);
          window.__attempts[0].emit('message', bytes.buffer);
        }""")
        self.assertEqual(await self.page.locator('#runtime-ticks').inner_text(), '0.0M')
        self.assertEqual(await self.page.evaluate("Array.from(document.querySelector('#lcd').getContext('2d').getImageData(0,0,1,1).data)"), [202, 213, 167, 255])
        await self.emit(self.runtime_state(run_id="33" * 16, generation=4, height=80))
        self.assertEqual(await self.page.locator('#lcd').get_attribute('height'), '80')
        self.assertEqual(self.errors, [])

    async def test_unidentified_bridge_restart_preserves_last_display(self):
        await self.load('silent')
        await self.emit(self.runtime_state(height=80))
        await self.page.evaluate("""() => {
          const canvas = document.querySelector('#lcd');
          const context = canvas.getContext('2d');
          context.fillStyle = '#ff0000'; context.fillRect(0, 0, canvas.width, canvas.height);
          window.__attempts[0].emit('error');
        }""")
        await self.page.wait_for_function("() => window.__attempts.length === 2 && window.__attempts[1].readyState === 1")
        await asyncio.sleep(0.1)
        self.assertEqual(await self.page.locator('#lcd').get_attribute('height'), '80')
        self.assertEqual(await self.page.locator('#runtime-ticks').inner_text(), '1.0M')
        self.assertEqual(await self.page.evaluate("Array.from(document.querySelector('#lcd').getContext('2d').getImageData(0,0,1,1).data)"), [255, 0, 0, 255])
        await self.start_emulator()
        await self.assert_snapshot()
        self.assertEqual(self.errors, [])

    async def test_retired_filesystem_response_cannot_repopulate_replacement(self):
        await self.load('silent')
        await self.page.evaluate("""() => {
          window.__fileCalls = [];
          window.fetch = async url => {
            window.__fileCalls.push(url);
            if (url.includes('/list')) return await new Promise(resolve => window.__oldListing = resolve);
            return new Response(JSON.stringify({state: window.__fileCalls.length === 1 ? 'ready' : 'disconnected'}),
              {status: 200, headers: {'Content-Type': 'application/json'}});
          };
        }""")
        await self.emit(self.runtime_state(asc0_available=True))
        await self.page.wait_for_function('Boolean(window.__oldListing)')
        await self.emit(self.runtime_state(run_id='22' * 16, generation=2, asc0_available=True))
        await self.page.evaluate("""() => window.__oldListing(new Response(JSON.stringify({path: 'A:',
            entries: [{kind: 'file', name: 'old-run.txt', path: 'A:\\old-run.txt', size: 1}]}),
            {status: 200, headers: {'Content-Type': 'application/json'}}))""")
        await self.emit({"type": "files_changed", "run_id": "11" * 16, "generation": 1})
        self.assertNotIn('old-run.txt', await self.page.locator('#files-list').inner_text())
        self.assertEqual(await self.page.locator('#files-status').inner_text(), 'Not connected')
        self.assertTrue(await self.page.locator('#files-refresh').is_disabled())
        self.assertEqual(self.errors, [])

    async def test_metrics_and_binary_cannot_validate_startup_or_replay_keys(self):
        await self.load('silent')
        state = self.runtime_state()
        await self.emit({"type": "metrics", "run_id": state["run_id"], "generation": 1, "stats": state["stats"]})
        await self.page.keyboard.down('1')
        self.assertEqual(await self.page.evaluate('window.__attempts[0].sent'), [])
        await self.emit(state)
        await self.page.keyboard.up('1')
        await self.page.keyboard.down('1')
        sent = await self.page.evaluate('window.__attempts[0].sent.map(value => JSON.parse(value))')
        self.assertEqual(sent[-1]["run_id"], state["run_id"])
        self.assertEqual(sent[-1]["generation"], 1)
        await self.page.keyboard.up('1')

    async def test_startup_open_without_state(self):
        await self.recover_stalled_socket('silent')

    async def test_startup_error_without_close(self):
        await self.load('silent')
        await self.start_emulator()
        await self.page.evaluate("window.__attempts[0].emit('error')")
        started = asyncio.get_running_loop().time()
        snapshot_time = await self.assert_snapshot()
        self.assertLess(snapshot_time - started, 1.5)
        await self.assert_retired_events_ignored()

    async def test_startup_open_and_nonstate_do_not_extend_deadline(self):
        await self.load('connecting')
        await self.start_emulator()
        started = asyncio.get_running_loop().time()
        await asyncio.sleep(4)
        await self.page.evaluate("""() => {
          const old = window.__attempts[0];
          old.readyState = 1;
          old.emit('open');
          old.emit('message', JSON.stringify({type: 'asc0_tx', text: 'not a state'}));
        }""")
        snapshot_time = await self.assert_snapshot()
        self.assertLess(snapshot_time - started, 6)

    async def test_startup_resume_checks_overdue_deadline(self):
        await self.load('silent', suspend_timer=True)
        await self.page.evaluate("""() => {
          Object.defineProperty(document, 'hidden', {configurable: true, value: true});
          document.dispatchEvent(new Event('visibilitychange'));
        }""")
        await self.start_emulator()
        await self.page.evaluate("""() => {
          window.__wallOffset = 6000;
          Object.defineProperty(document, 'hidden', {configurable: true, value: false});
          document.dispatchEvent(new Event('visibilitychange'));
        }""")
        self.assertEqual(await self.page.evaluate('window.__attempts[0].closeCalls'), 1)
        await self.assert_snapshot()
        self.assertEqual(await self.page.evaluate('window.__attempts.length'), 2)

    async def test_startup_disconnected_state_is_success(self):
        await self.load()
        await self.page.locator('#connection-state.error').wait_for()
        await asyncio.sleep(5.7)
        self.assertEqual(await self.page.evaluate('window.__attempts.length'), 1)
        await self.start_emulator()
        await self.assert_snapshot()
        await self.assert_advancing_stats()
        self.assertEqual(await self.page.evaluate('window.__attempts.length'), 1)

    async def check_teardown(self, pending_retry):
        await self.page.clock.install()
        await self.load('silent')
        if pending_retry:
            await self.page.evaluate("window.__attempts[0].emit('error')")
        await self.page.evaluate("window.dispatchEvent(new Event('pagehide'))")
        await self.page.clock.fast_forward(6000)
        self.assertEqual(await self.page.evaluate('window.__attempts.length'), 1)
        await self.page.evaluate("window.dispatchEvent(new Event('pageshow'))")
        await self.page.wait_for_function("window.__attempts.length === 2")
        await self.page.locator('#connection-state.error').wait_for()

    async def test_startup_teardown_cancels_startup(self):
        await self.check_teardown(False)

    async def test_startup_teardown_cancels_retry(self):
        await self.check_teardown(True)


if __name__ == "__main__":
    unittest.main()
