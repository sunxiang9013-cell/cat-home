import test from 'node:test';
import assert from 'node:assert/strict';
import { createFeeder, validSeconds } from '../model.js';

test('five estimated rotations finish at the calibrated duration', () => {
  const feeder = createFeeder();
  assert.equal(feeder.start(1, 1.2, 100), true);
  assert.equal(feeder.sample(3100).turns, 2.5);
  assert.equal(feeder.sample(6099).done, false);
  assert.equal(feeder.sample(6100).done, true);
  assert.equal(feeder.sample(9000).turns, 5);
});
test('repeated clicks and other channels cannot overlap an active operation', () => {
  const feeder = createFeeder();
  feeder.start(2, 1, 0);
  assert.equal(feeder.start(2, 1, 100), false);
  assert.equal(feeder.start(3, 1, 100), false);
  assert.equal(feeder.sample(100).channel, 2);
});
test('stop cancels an operation and allows another channel', () => {
  const feeder = createFeeder();
  feeder.start(1, 1, 0);
  assert.equal(feeder.stop().channel, 1);
  assert.equal(feeder.sample(99999), null);
  assert.equal(feeder.start(3, 2, 100), true);
  assert.equal(feeder.sample(10100).done, true);
});
test('invalid calibration and channels cannot start a timed operation', () => {
  for (const value of [NaN, Infinity, -1, 0, 0.19, 30.1]) {
    assert.equal(validSeconds(value), false);
    assert.equal(createFeeder().start(1, value, 0), false);
  }
  for (const channel of [0, 4, 1.5, NaN]) assert.equal(createFeeder().start(channel, 1, 0), false);
});
