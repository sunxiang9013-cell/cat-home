import test from 'node:test';
import assert from 'node:assert/strict';
import { FrameDecoder } from '../frame-stream.js';

const jpeg = Uint8Array.from([255, 216, 1, 2, 255, 217]);
const packet = Uint8Array.from([0, 0, 0, jpeg.length, ...jpeg]);
test('continuous video accepts any split across the header and JPEG', () => {
  for (let split = 1; split < packet.length; split++) {
    const decoder = new FrameDecoder();
    assert.deepEqual(decoder.push(packet.slice(0, split)), []);
    assert.deepEqual(decoder.push(packet.slice(split)), [jpeg]);
    assert.equal(decoder.buffer.length, 0);
  }
});
test('batched frames are extracted in order so the viewer can discard old ones', () => {
  const decoder = new FrameDecoder();
  const nextJpeg = Uint8Array.from([255, 216, 9, 255, 217]);
  const combined = Uint8Array.from([...packet, 0, 0, 0, nextJpeg.length, ...nextJpeg, 0, 0]);
  assert.deepEqual(decoder.push(combined), [jpeg, nextJpeg]);
  assert.equal(decoder.buffer.length, 2);
});
test('invalid lengths and corrupt images are rejected before display', () => {
  assert.throws(() => new FrameDecoder().push(Uint8Array.from([255, 255, 255, 255])), /长度异常/);
  assert.throws(() => new FrameDecoder().push(Uint8Array.from([0, 0, 0, 4, 1, 2, 3, 4])), /不完整/);
});
