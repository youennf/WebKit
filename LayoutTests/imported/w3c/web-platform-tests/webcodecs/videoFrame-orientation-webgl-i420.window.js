// META: script=/webcodecs/videoFrame-orientation-utils.js

// On some platforms texImage2D() of a YUV VideoFrame is done on the GPU, without
// converting the frame to RGB first. Check that the orientation is applied there too.
//
// A 4x4 I420 frame made of four 2x2 blocks, so that chroma subsampling does not blend colors:
//   y r
//   b g
const kBlocks = [
  ['y', 'r'],
  ['b', 'g'],
];

// BT.709, limited range.
function rgbToYuv([r, g, b]) {
  const kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
  const y = (kr * r + kg * g + kb * b) / 255;
  const u = (b / 255 - y) / (2 * (1 - kb));
  const v = (r / 255 - y) / (2 * (1 - kr));
  return [16 + 219 * y, 128 + 224 * u, 128 + 224 * v].map(Math.round);
}

function makeI420Frame(rotation) {
  const size = 4;
  const data = new Uint8Array(size * size * 3 / 2);
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x)
      data[y * size + x] = rgbToYuv(kOrientationColors[kBlocks[y >> 1][x >> 1]])[0];
  }
  for (let blockY = 0; blockY < 2; ++blockY) {
    for (let blockX = 0; blockX < 2; ++blockX) {
      const [, u, v] = rgbToYuv(kOrientationColors[kBlocks[blockY][blockX]]);
      data[size * size + blockY * 2 + blockX] = u;
      data[size * size + 4 + blockY * 2 + blockX] = v;
    }
  }
  return new VideoFrame(data, {
    format: 'I420',
    codedWidth: size,
    codedHeight: size,
    timestamp: 0,
    rotation,
  });
}

// The name of the color of the block rendered at the pixel (x, y), the blocks being rotated clockwise.
function expectedBlockName(rotation, x, y) {
  const blockX = x >> 1;
  const blockY = y >> 1;
  switch (rotation) {
    case 0: return kBlocks[blockY][blockX];
    case 90: return kBlocks[1 - blockX][blockY];
    case 180: return kBlocks[1 - blockY][1 - blockX];
    case 270: return kBlocks[blockX][1 - blockY];
  }
  assert_unreached('unexpected rotation');
}

for (const {rotation} of kOrientationCombinations) {
  promise_test(async t => {
    const frame = makeI420Frame(rotation);
    t.add_cleanup(() => frame.close());
    assert_equals(frame.rotation, rotation, 'rotation');
    assert_equals(frame.displayWidth, 4, 'displayWidth');
    assert_equals(frame.displayHeight, 4, 'displayHeight');

    const pixels = readTexturePixels(frame, 4, 4);
    for (let y = 0; y < 4; ++y) {
      for (let x = 0; x < 4; ++x) {
        const name = expectedBlockName(rotation, x, y);
        const expected = kOrientationColors[name];
        const actual = Array.from(pixels.subarray(4 * (y * 4 + x), 4 * (y * 4 + x) + 3));
        for (let i = 0; i < 3; ++i) {
          assert_approx_equals(actual[i], expected[i], 8,
              `pixel (${x}, ${y}) should be ${name} [${expected}] but is [${actual}]`);
        }
      }
    }
  }, `texImage2D() of a ${rotation} degrees rotated I420 VideoFrame`);
}
