"""Real browser encoding/decoding and capture lifecycle regressions."""
import asyncio
import math
import struct
import unittest

from .. import app as web_ui
from . import test_browser


class CaptureTests(unittest.IsolatedAsyncioTestCase):
    # Reuse the bridge/browser fixture, but load capture with native Web Audio.
    async def asyncSetUp(self):
        await test_browser.BrowserUITests.asyncSetUp(self)
        await self.context.close()
        self.context = await self.browser.new_context(viewport={"width": 1280, "height": 900})
        await self.context.add_init_script("""(() => {
          window.__downloads = [];
          window.__recorders = [];
          window.__captureCanvases = [];
          window.__streams = [];
          const capture = HTMLCanvasElement.prototype.captureStream;
          HTMLCanvasElement.prototype.captureStream = function(rate) {
            const stream = capture.call(this, rate);
            __captureCanvases.push(this); __streams.push(stream); return stream;
          };
          const create = URL.createObjectURL.bind(URL);
          const revoke = URL.revokeObjectURL.bind(URL);
          window.__urls = new Map();
          URL.createObjectURL = blob => { const url = create(blob); __urls.set(url, blob); return url; };
          URL.revokeObjectURL = url => { __urls.delete(url); revoke(url); };
          const click = HTMLAnchorElement.prototype.click;
          HTMLAnchorElement.prototype.click = function() {
            if (this.download) __downloads.push({blob: __urls.get(this.href), name: this.download});
            click.call(this);
          };
          const NativeRecorder = window.MediaRecorder;
          window.MediaRecorder = class extends NativeRecorder {
            constructor(stream, options) { super(stream, options); __recorders.push(this); }
          };
        })()""")
        self.page = await self.context.new_page()
        self.console_errors = []
        self.page.on("pageerror", lambda error: self.console_errors.append(str(error)))
        await self.page.goto(f"http://127.0.0.1:{self.web.port}/")
        await self.page.wait_for_function("() => !document.querySelector('#capture-record').disabled")

    asyncTearDown = test_browser.BrowserUITests.asyncTearDown

    async def start_recording(self):
        await self.page.click("#capture-record")
        await self.page.wait_for_function("() => document.querySelector('#capture-toolbar').dataset.state === 'recording'")

    async def stop_recording(self, count=1):
        await self.page.click("#capture-record")
        await self.wait_downloads(count)

    async def wait_downloads(self, count):
        await self.page.wait_for_function("n => __downloads.length >= n", arg=count)
        await self.page.wait_for_function("() => document.querySelector('#capture-toolbar').dataset.state === 'idle'")

    async def assert_released(self):
        self.assertTrue(await self.page.evaluate("__streams.every(s => s.getTracks().every(t => t.readyState === 'ended'))"))
        self.assertEqual(self.console_errors, [])

    async def test_png_exact_pixels_recording_audio_and_repeated_clips(self):
        await self.start_recording()
        timer_layout = await self.page.evaluate("""() => {
          const button = document.querySelector('#capture-record').getBoundingClientRect();
          const timer = document.querySelector('#capture-timer').getBoundingClientRect();
          return {gap:timer.top-button.bottom, left:timer.left, right:timer.right};
        }""")
        self.assertAlmostEqual(timer_layout['gap'], 4, delta=1)
        self.assertGreaterEqual(timer_layout['left'], 0)
        self.assertLessEqual(timer_layout['right'], 52)
        pcm = struct.pack("<IHH", 48000, 1, 0) + b"".join(
            struct.pack("<h", int(16000 * math.sin(i * 2 * math.pi * 440 / 48000)))
            for i in range(4800))
        for _ in range(6):
            self.fake.writer.write(web_ui.encode_packet(web_ui.SPEAKER_PCM, pcm))
            await self.fake.writer.drain()
            await asyncio.sleep(.09)
        async with self.page.expect_download() as download:
            await self.page.click("#capture-screenshot")
        self.assertRegex((await download.value).suggested_filename, r"M55-.*\.png$")
        png = await self.page.evaluate("""async () => {
          const bitmap = await createImageBitmap(__downloads[0].blob);
          const canvas = document.createElement('canvas');
          canvas.width = bitmap.width; canvas.height = bitmap.height;
          const ctx = canvas.getContext('2d'); ctx.drawImage(bitmap, 0, 0);
          const actual = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
          const expected = document.querySelector('#lcd').getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
          return [bitmap.width, bitmap.height, actual.every((v, i) => v === expected[i])];
        }""")
        self.assertEqual(png, [101, 80, True])
        self.assertTrue(await self.page.evaluate("""() => {
          const source = document.querySelector('#lcd');
          const target = __captureCanvases[0];
          const src = source.getContext('2d').getImageData(0, 0, 101, 80).data;
          const dst = target.getContext('2d').getImageData(0, 0, 404, 320).data;
          return dst.every((v, i) => {
            const pixel = Math.floor(i / 4), channel = i % 4;
            const x = Math.floor((pixel % 404) / 4), y = Math.floor(pixel / 404 / 4);
            return v === src[(y * 101 + x) * 4 + channel];
          });
        }"""))
        self.fake.writer.write(web_ui.encode_packet(web_ui.FRAME, bytes([230, 30, 20]) * (101 * 80)))
        await self.fake.writer.drain()
        await asyncio.sleep(.65)
        await self.stop_recording(2)
        encoded = await self.page.evaluate("async () => [...new Uint8Array(await __downloads[1].blob.arrayBuffer())]")
        decoder = await self.context.new_page()
        result = await decoder.evaluate("""async bytes => {
          const blob = new Blob([new Uint8Array(bytes)], {type:'video/webm'});
          const video = document.createElement('video');
          video.muted = true;
          video.src = URL.createObjectURL(blob);
          await new Promise((resolve, reject) => { video.onloadeddata = resolve; video.onerror = () => reject(new Error('decode load failed')); setTimeout(() => reject(new Error('decode load timeout')), 5000); });
          const canvas = document.createElement('canvas');
          canvas.width = video.videoWidth; canvas.height = video.videoHeight;
          const ctx = canvas.getContext('2d');
          async function pixel(time) {
            await new Promise((resolve, reject) => { video.onseeked = resolve; video.currentTime = time; setTimeout(() => reject(new Error(`seek timeout ${time}, duration ${video.duration}, state ${video.readyState}`)), 5000); });
            ctx.drawImage(video, 0, 0); return [...ctx.getImageData(20, 20, 1, 1).data];
          }
          const first = await pixel(.2), last = await pixel(1.1);
          URL.revokeObjectURL(video.src);
          const audio = new AudioContext();
          const decoded = await audio.decodeAudioData(await blob.arrayBuffer());
          const peak = Math.max(...decoded.getChannelData(0).subarray(0, 48000).map(Math.abs));
          await audio.close();
          return {width: canvas.width, height: canvas.height, first, last, peak};
        }""", encoded)
        await decoder.close()
        self.assertEqual((result['width'], result['height']), (404, 320))
        for actual, expected in zip(result['first'][:3], [40, 60, 35]):
            self.assertAlmostEqual(actual, expected, delta=12)
        self.assertGreater(result['last'][0], 200)
        self.assertLess(result['last'][1], 50)
        self.assertGreater(result['peak'], .1)
        await self.assert_released()
        await self.start_recording()
        await asyncio.sleep(.3)
        await self.stop_recording(3)
        await self.assert_static_clip(2)
        await self.start_recording()
        await self.stop_recording(4)
        await self.assert_static_clip(3)
        await self.assert_released()

    async def assert_static_clip(self, index, dimensions=(404, 320), rgb=(230, 30, 20)):
        encoded = await self.page.evaluate("async i => [...new Uint8Array(await __downloads[i].blob.arrayBuffer())]", index)
        decoder = await self.context.new_page()
        pixels = await decoder.evaluate("""async bytes => {
          const video = document.createElement('video');
          video.src = URL.createObjectURL(new Blob([new Uint8Array(bytes)], {type:'video/webm'}));
          await new Promise((resolve, reject) => {
            video.onloadeddata = resolve; video.onerror = () => reject(new Error('invalid static clip'));
            setTimeout(() => reject(new Error('static clip load timeout')), 5000);
          });
          const canvas = document.createElement('canvas'); canvas.width=404; canvas.height=320;
          const ctx = canvas.getContext('2d');
          // WebM audio can begin a few milliseconds before the first video PTS.
          await new Promise((resolve, reject) => {
            video.onseeked = resolve; video.currentTime = .05;
            setTimeout(() => reject(new Error('initial static frame seek timeout')), 5000);
          });
          ctx.drawImage(video, 0, 0);
          const first = [...ctx.getImageData(20,20,1,1).data];
          await new Promise((resolve, reject) => {
            video.onseeked = resolve; video.currentTime = .1;
            setTimeout(() => reject(new Error('static clip seek timeout')), 5000);
          });
          ctx.drawImage(video, 0, 0);
          const last = [...ctx.getImageData(20,20,1,1).data];
          URL.revokeObjectURL(video.src);
          return [video.videoWidth, video.videoHeight, first, last];
        }""", encoded)
        await decoder.close()
        self.assertEqual(pixels[:2], list(dimensions))
        for pixel in pixels[2:]:
            for actual, expected in zip(pixel[:3], rgb):
                self.assertAlmostEqual(actual, expected, delta=12)

    async def test_disconnect_retains_screenshot_and_keyboard_isolation(self):
        await self.page.focus('#capture-screenshot')
        await self.page.keyboard.press('ArrowUp')
        await self.page.keyboard.press('1')
        await asyncio.sleep(.05)
        controls = []
        while not self.fake.received.empty():
            controls.append(self.fake.received.get_nowait())
        self.assertFalse(any(p.message_type == web_ui.KEY for p in controls))
        await self.start_recording()
        await asyncio.sleep(.15)
        await self.fake.stop()
        await self.wait_downloads(1)
        self.assertTrue(await self.page.is_disabled('#capture-record'))
        self.assertFalse(await self.page.is_disabled('#capture-screenshot'))
        await self.page.focus('#capture-screenshot')
        async with self.page.expect_download():
            await self.page.keyboard.press('Enter')
        await self.assert_released()

    async def test_toolbar_geometry_and_mobile_files_keep_recording(self):
        for width, height in [(1280, 900), (801, 900), (800, 900), (390, 844), (280, 700)]:
            await self.page.set_viewport_size({'width': width, 'height': height})
            for lcd_width, lcd_height in [(101, 64), (101, 80), (132, 176)]:
                await self.page.evaluate("""([w,h]) => {
                  const c = document.querySelector('#lcd'); c.width=w; c.height=h;
                  c.style.aspectRatio = `${w}/${h}`;
                }""", [lcd_width, lcd_height])
                for dock in ['asc0-visible', 'asc0-minimized', 'asc0-unavailable']:
                    await self.page.evaluate("c => document.body.className = c", dock)
                    await self.page.wait_for_timeout(50)
                    layout = await self.page.evaluate("""() => {
                      const p = document.querySelector('.phone').getBoundingClientRect();
                      const t = document.querySelector('#capture-toolbar').getBoundingClientRect();
                      return {left:t.left, width:t.width, height:t.height, center:t.top+t.height/2,
                        phoneCenter:p.top+p.height/2, phoneLeft:p.left, right:p.right, scroll:document.body.scrollWidth};
                    }""")
                    self.assertEqual(layout['left'], 0)
                    self.assertEqual(layout['width'], 52)
                    self.assertLessEqual(layout['height'], 240)
                    self.assertAlmostEqual(layout['center'], layout['phoneCenter'], delta=1)
                    self.assertGreaterEqual(layout['phoneLeft'], 52)
                    self.assertLessEqual(layout['right'], width)
                    self.assertLessEqual(layout['scroll'], width)
        await self.page.evaluate("document.body.className = 'asc0-visible'")
        await self.start_recording()
        await self.page.click('#tab-files')
        self.assertFalse(await self.page.is_visible('#capture-toolbar'))
        await asyncio.sleep(.25)
        self.assertEqual(await self.page.get_attribute('#capture-toolbar', 'data-state'), 'recording')
        other = await self.context.new_page()
        await other.bring_to_front()
        await asyncio.sleep(.3)
        self.assertEqual(await self.page.get_attribute('#capture-toolbar', 'data-state'), 'recording')
        await other.close()
        await self.page.bring_to_front()
        await self.page.click('#tab-phone')
        await self.stop_recording()
        await self.assert_released()

    async def isolated_controller(self, options=None, prelude=""):
        await self.page.route('**/app.js', lambda route: route.fulfill(body='', content_type='text/javascript'))
        await self.page.reload()
        if prelude:
            await self.page.evaluate(prelude if prelude.startswith("() =>") else "() => {" + prelude + ";}")
        await self.page.evaluate("""async options => {
          const {createCapture} = await import('/capture.js');
          const {createPhone} = await import('/phone.js');
          window.__phone = createPhone();
          __phone.updateState({model:'c55', width:101, height:64});
          __phone.drawFrame(new Uint8Array(101*64*3).fill(80).buffer);
          window.__capture = createCapture({phone:__phone,
            audio: window.__testAudio || {acquireFeed: async () => null}, ...options});
          window.__state = {bridge:'connected', run_id:'one', generation:1, width:101, height:64};
          __capture.updateState(__state);
        }""", options or {})

    async def test_limits_and_video_only(self):
        for options, explanation in [({'maxDurationMs': 180}, '10-minute'), ({'maxBytes': 1}, '128 MiB')]:
            await self.isolated_controller(options)
            await self.start_recording()
            await self.wait_downloads(1)
            self.assertIn(explanation, await self.page.inner_text('#capture-notice'))
            self.assertEqual(await self.page.evaluate('__recorders[0].stream.getAudioTracks().length'), 0)
            await self.assert_static_clip(0, dimensions=(404, 256), rgb=(80, 80, 80))
            await self.assert_released()

    async def test_replacement_geometry_and_termination(self):
        for update, explanation in [({'generation':2}, 'attachment'),
                                    ({'run_id':'two'}, 'attachment'),
                                    ({'height':80}, 'dimensions'),
                                    ({'lifecycle':{'phase':'stopped'}}, 'stopped')]:
            await self.isolated_controller()
            await self.start_recording()
            await asyncio.sleep(.15)
            await self.page.evaluate('patch => __capture.updateState({...__state, ...patch})', update)
            await self.wait_downloads(1)
            self.assertIn(explanation, await self.page.inner_text('#capture-notice'))
            await self.assert_released()

    async def test_capabilities_errors_and_interrupted_startup(self):
        for prelude, explanation in [
            ('window.MediaRecorder = undefined', 'WebM'),
            ('HTMLCanvasElement.prototype.captureStream = undefined', 'canvas'),
            ("MediaRecorder.isTypeSupported = () => false", 'WebM'),
        ]:
            await self.isolated_controller(prelude=prelude)
            self.assertTrue(await self.page.is_disabled('#capture-record'))
            self.assertFalse(await self.page.is_disabled('#capture-screenshot'))
            self.assertIn(explanation, await self.page.inner_text('#capture-notice'))
        for prelude, explanation in [
            ("window.__testAudio = {acquireFeed: async () => {throw new Error('audio init failed')}}", 'audio init failed'),
            ("HTMLCanvasElement.prototype.captureStream = () => {throw new Error('canvas failed')}", 'canvas failed'),
            ("window.MediaRecorder = class {static isTypeSupported() {return true} constructor() {throw new Error('encoder failed')}}", 'encoder failed'),
        ]:
            await self.isolated_controller(prelude=prelude)
            await self.page.click('#capture-record')
            await self.page.wait_for_function("() => document.querySelector('#capture-toolbar').dataset.state === 'idle'")
            self.assertIn(explanation, await self.page.inner_text('#capture-notice'))
            await self.assert_released()
        for interrupt in ['button', 'disconnect', 'replacement']:
            await self.isolated_controller(prelude="""() => {
              window.__released = 0;
              window.__testAudio = {acquireFeed: () => new Promise(resolve => {
                window.__resolveFeed = () => resolve({stream:new MediaStream(), release() {__released++}});
              })};
            }""")
            await self.page.click('#capture-record')
            self.assertEqual(await self.page.get_attribute('#capture-toolbar', 'data-state'), 'starting')
            if interrupt == 'button':
                await self.page.click('#capture-record')
            elif interrupt == 'disconnect':
                await self.page.evaluate('__capture.disconnect()')
            else:
                await self.page.evaluate('__capture.updateState({...__state, generation:2})')
            await self.page.evaluate('__resolveFeed()')
            await self.page.wait_for_function('() => __released === 1')
            self.assertEqual(await self.page.evaluate('__recorders.length'), 0)
            self.assertEqual(await self.page.get_attribute('#capture-toolbar', 'data-state'), 'idle')
            self.assertEqual(await self.page.evaluate('__downloads.length'), 0)

    async def test_real_audio_failure_is_visible(self):
        await self.page.evaluate("() => {AudioContext.prototype.createMediaStreamDestination = () => {throw new Error('feed unavailable')}}")
        await self.page.click('#capture-record')
        await self.page.wait_for_function("() => document.querySelector('#capture-notice').textContent.includes('Audio capture could not start')")
        self.assertEqual(await self.page.get_attribute('#capture-toolbar', 'data-state'), 'idle')
        self.assertEqual(self.console_errors, [])

    async def test_frame_readiness_encoder_error_and_url_cleanup(self):
        await self.isolated_controller()
        await self.page.evaluate('__phone.clear(); __capture.refresh()')
        self.assertTrue(await self.page.is_disabled('#capture-screenshot'))
        self.assertTrue(await self.page.is_disabled('#capture-record'))
        await self.page.evaluate('__phone.drawFrame(new Uint8Array(3).buffer); __capture.refresh()')
        self.assertTrue(await self.page.is_disabled('#capture-record'))
        await self.page.evaluate('__phone.drawFrame(new Uint8Array(101*64*3).buffer); __capture.refresh()')
        await self.start_recording()
        await asyncio.sleep(.25)
        await self.page.evaluate("__recorders[0].dispatchEvent(new Event('error'))")
        await self.wait_downloads(1)
        self.assertIn('Recording failed', await self.page.inner_text('#capture-notice'))
        await self.assert_released()
        self.assertEqual(await self.page.evaluate('__urls.size'), 1)
        await self.page.evaluate("window.dispatchEvent(new Event('pagehide'))")
        self.assertEqual(await self.page.evaluate('__urls.size'), 0)
