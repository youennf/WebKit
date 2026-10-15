// META: global=window,dedicatedworker

// Checks the rendering of rotated and flipped VideoFrames, in particular that flip is applied after
// rotation: the image is first rotated clockwise, then mirrored horizontally.
//
// The source image is
//   y y r r
//   b b g g

const kColors = {
  y: [255, 255, 0],
  r: [255, 0, 0],
  b: [0, 0, 255],
  g: [0, 255, 0],
};
const kSource = [
  ['y', 'y', 'r', 'r'],
  ['b', 'b', 'g', 'g'],
];

// Pixel grid operations. A grid is an array of rows.
function rotateClockwise(grid) {
  const height = grid.length;
  const width = grid[0].length;
  return Array.from({length: width}, (_, y) => Array.from({length: height}, (_, x) => grid[height - 1 - x][y]));
}

function rotate(grid, degrees) {
  for (let i = 0; i < degrees / 90; ++i)
    grid = rotateClockwise(grid);
  return grid;
}

function flipHorizontally(grid) {
  return grid.map(row => row.toReversed());
}

function render(grid, {rotation, flip}) {
  const rotated = rotate(grid, rotation);
  return flip ? flipHorizontally(rotated) : rotated;
}

function makeFrame({rotation, flip}) {
  const data = new Uint8Array(kSource[0].length * kSource.length * 4);
  kSource.forEach((row, y) => row.forEach((name, x) => {
    data.set([...kColors[name], 255], 4 * (y * row.length + x));
  }));
  return new VideoFrame(data, {
    format: 'RGBA',
    codedWidth: kSource[0].length,
    codedHeight: kSource.length,
    timestamp: 0,
    rotation,
    flip,
  });
}

function drawn(frame, grid) {
  const height = grid.length;
  const width = grid[0].length;
  assert_equals(frame.displayWidth, width, 'displayWidth');
  assert_equals(frame.displayHeight, height, 'displayHeight');
  const ctx = new OffscreenCanvas(width, height).getContext('2d');
  ctx.drawImage(frame, 0, 0);
  const data = ctx.getImageData(0, 0, width, height).data;
  return grid.map((row, y) => row.map((_, x) => Array.from(data.subarray(4 * (y * width + x), 4 * (y * width + x) + 3))));
}

function verify(frame, expectedGrid, message) {
  const pixels = drawn(frame, expectedGrid);
  expectedGrid.forEach((row, y) => row.forEach((name, x) => {
    pixels[y][x].forEach((value, i) => {
      assert_approx_equals(value, kColors[name][i], 2,
          `${message}: pixel (${x}, ${y}) should be ${name} [${kColors[name]}] but is [${pixels[y][x]}]`);
    });
  }));
}

function sameGrids(a, b) {
  return JSON.stringify(a) === JSON.stringify(b);
}

for (const flip of [false, true]) {
  for (const rotation of [0, 90, 180, 270]) {
    test(() => {
      const frame = makeFrame({rotation, flip});
      verify(frame, render(kSource, {rotation, flip}), 'frame');
      frame.close();
    }, `Rendering of a VideoFrame with a rotation of ${rotation} and flip ${flip}`);
  }
}

// Rotating then flipping is not the same as flipping then rotating by the same angle, except for
// 0 and 180 degrees. Make sure the test distinguishes both.
for (const rotation of [90, 270]) {
  test(() => {
    const flipThenRotate = rotate(flipHorizontally(kSource), rotation);
    const rotateThenFlip = render(kSource, {rotation, flip: true});
    assert_false(sameGrids(flipThenRotate, rotateThenFlip), 'the two orders give different images');

    const frame = makeFrame({rotation, flip: true});
    const pixels = drawn(frame, rotateThenFlip);
    frame.close();
    const toNames = grid => grid.map(row => row.map(pixel => Object.keys(kColors).find(name => kColors[name].every((value, i) => Math.abs(value - pixel[i]) <= 2))));
    assert_array_equals(toNames(pixels).flat(), rotateThenFlip.flat(), 'flip is applied after the rotation');
    assert_not_equals(toNames(pixels).flat().join(), flipThenRotate.flat().join(), 'flip is not applied before the rotation');
  }, `Flip is applied after a rotation of ${rotation}`);
}

// A frame created from a flipped frame: the rotation of the init is applied before the flip of the base frame.
for (const rotation of [90, 270]) {
  test(() => {
    const baseFrame = makeFrame({rotation: 0, flip: true});
    const frame = new VideoFrame(baseFrame, {rotation});
    // The flipped image is rotated, then there is nothing to flip back: the result is the flipped source rotated.
    const expected = rotate(flipHorizontally(kSource), rotation);
    verify(frame, expected, 'derived frame');
    frame.close();
    baseFrame.close();
  }, `A VideoFrame created from a flipped VideoFrame with an init rotation of ${rotation} rotates the flipped image`);

  test(() => {
    const baseFrame = makeFrame({rotation, flip: false});
    const frame = new VideoFrame(baseFrame, {flip: true});
    // The rotated image is flipped.
    verify(frame, flipHorizontally(rotate(kSource, rotation)), 'derived frame');
    frame.close();
    baseFrame.close();
  }, `A VideoFrame created from a VideoFrame rotated by ${rotation} with an init flip flips the rotated image`);
}
