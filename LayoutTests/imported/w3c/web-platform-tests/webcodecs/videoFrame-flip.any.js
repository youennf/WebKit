// META: global=window,dedicatedworker

// Flip is applied after rotation: the frame is first rotated clockwise, then mirrored horizontally.
// Creating a frame from a flipped frame with a rotation rotates the frame before the flip is applied,
// i.e. the rotation is subtracted from the base rotation.

const kWidth = 4;
const kHeight = 2;

function makeFrame({rotation, flip} = {}) {
  const init = {format: 'RGBA', codedWidth: kWidth, codedHeight: kHeight, timestamp: 0};
  if (rotation !== undefined)
    init.rotation = rotation;
  if (flip !== undefined)
    init.flip = flip;
  return new VideoFrame(new Uint8Array(kWidth * kHeight * 4), init);
}

function verifyFrame(frame, {rotation, flip}, message) {
  assert_equals(frame.rotation, rotation, `${message}: rotation`);
  assert_equals(frame.flip, flip, `${message}: flip`);
  const isSwapped = rotation % 180 !== 0;
  assert_equals(frame.displayWidth, isSwapped ? kHeight : kWidth, `${message}: displayWidth`);
  assert_equals(frame.displayHeight, isSwapped ? kWidth : kHeight, `${message}: displayHeight`);
  assert_equals(frame.codedWidth, kWidth, `${message}: codedWidth`);
  assert_equals(frame.codedHeight, kHeight, `${message}: codedHeight`);
}

const kRotations = [0, 90, 180, 270];

test(() => {
  const frame = makeFrame();
  assert_equals(frame.flip, false);
  frame.close();
}, 'A VideoFrame is not flipped by default');

for (const flip of [false, true]) {
  for (const rotation of kRotations) {
    test(() => {
      const frame = makeFrame({rotation, flip});
      verifyFrame(frame, {rotation, flip}, 'frame');
      frame.close();
    }, `A VideoFrame created from data with a rotation of ${rotation} and flip ${flip}`);
  }
}

for (const baseFlip of [false, true]) {
  for (const baseRotation of kRotations) {
    for (const deltaFlip of [false, true]) {
      for (const deltaRotation of kRotations) {
        test(() => {
          const baseFrame = makeFrame({rotation: baseRotation, flip: baseFlip});
          const frame = new VideoFrame(baseFrame, {rotation: deltaRotation, flip: deltaFlip});
          // The rotation of the init is applied to the image before the flip of the base frame.
          const rotation = ((baseFlip ? baseRotation - deltaRotation : baseRotation + deltaRotation) + 360) % 360;
          verifyFrame(frame, {rotation, flip: baseFlip !== deltaFlip}, 'derived frame');
          verifyFrame(baseFrame, {rotation: baseRotation, flip: baseFlip}, 'base frame');
          frame.close();
          baseFrame.close();
        }, `A VideoFrame created from a VideoFrame with a rotation of ${baseRotation} and flip ${baseFlip} with an init rotation of ${deltaRotation} and flip ${deltaFlip}`);
      }
    }
  }
}

// Explicit cases showing the order of the operations.
test(() => {
  // The base frame is rotated by 90 degrees then flipped. Rotating it by 90 degrees more in the
  // other direction (the rotation of the init is applied before the flip) gives no rotation.
  const baseFrame = makeFrame({rotation: 90, flip: true});
  const frame = new VideoFrame(baseFrame, {rotation: 90});
  verifyFrame(frame, {rotation: 0, flip: true}, 'derived frame');
  frame.close();
  baseFrame.close();
}, 'A rotation of the init is applied before the flip of the base frame');

test(() => {
  const baseFrame = makeFrame({rotation: 90, flip: false});
  const frame = new VideoFrame(baseFrame, {rotation: 90});
  verifyFrame(frame, {rotation: 180, flip: false}, 'derived frame');
  frame.close();
  baseFrame.close();
}, 'A rotation of the init is added to the rotation of a non flipped base frame');

test(() => {
  // Flipping a frame twice gives the frame back, flip does not depend on the rotation.
  const baseFrame = makeFrame({rotation: 270, flip: true});
  const frame = new VideoFrame(baseFrame, {flip: true});
  verifyFrame(frame, {rotation: 270, flip: false}, 'derived frame');
  frame.close();
  baseFrame.close();
}, 'Flipping a flipped VideoFrame removes the flip and keeps the rotation');

test(() => {
  const baseFrame = makeFrame({rotation: 90, flip: true});
  const frame = new VideoFrame(baseFrame);
  verifyFrame(frame, {rotation: 90, flip: true}, 'derived frame');
  frame.close();
  baseFrame.close();
}, 'A VideoFrame created from a flipped VideoFrame without init keeps rotation and flip');

test(() => {
  const baseFrame = makeFrame({rotation: 90, flip: true});
  const clone = baseFrame.clone();
  verifyFrame(clone, {rotation: 90, flip: true}, 'clone');
  const frame = new VideoFrame(clone, {rotation: 90, flip: true});
  verifyFrame(frame, {rotation: 0, flip: false}, 'derived frame');
  verifyFrame(clone, {rotation: 90, flip: true}, 'clone after deriving');
  verifyFrame(baseFrame, {rotation: 90, flip: true}, 'base frame after deriving');
  frame.close();
  clone.close();
  baseFrame.close();
}, 'Cloning a flipped VideoFrame keeps its flip, and deriving from the clone does not change it');

test(() => {
  const frame = makeFrame({rotation: 90, flip: true});
  frame.close();
  assert_equals(frame.rotation, 0, 'rotation');
  assert_equals(frame.flip, false, 'flip');
}, 'A closed VideoFrame has no rotation and no flip');

promise_test(async t => {
  const data = new Uint8Array(kWidth * kHeight * 4).map((_, i) => i * 7);
  const frame = new VideoFrame(data, {format: 'RGBA', codedWidth: kWidth, codedHeight: kHeight, timestamp: 0, rotation: 90, flip: true});
  t.add_cleanup(() => frame.close());
  const copy = new Uint8Array(frame.allocationSize());
  await frame.copyTo(copy);
  assert_array_equals(copy, data);
}, 'copyTo() of a rotated and flipped VideoFrame is not affected by its rotation and flip');
