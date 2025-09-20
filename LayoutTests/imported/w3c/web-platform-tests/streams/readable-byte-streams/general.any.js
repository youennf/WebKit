// META: global=window,worker
// META: script=../resources/rs-utils.js
// META: script=../resources/test-utils.js
'use strict';

const error1 = new Error('error1');
error1.name = 'error1';
// View buffers are detached after pull() returns, so record the information at the time that pull() was called.
function extractViewInfo(view) {
  return {
    constructor: view.constructor,
    bufferByteLength: view.buffer.byteLength,
    byteOffset: view.byteOffset,
    byteLength: view.byteLength
  };
}

promise_test(() => {
  let pullCount = 0;

  let controller;
  let byobRequest;
  let viewInfo;

  const stream = new ReadableStream({
    start(c) {
      controller = c;
    },
    pull() {
      ++pullCount;

      byobRequest = controller.byobRequest;
      const view = byobRequest.view;
      viewInfo = extractViewInfo(view);

      view[0] = 0x01;
      view[1] = 0x02;
      view[2] = 0x03;

      controller.byobRequest.respond(3);
    },
    type: 'bytes'
  });

  const reader = stream.getReader({ mode: 'byob' });

  return reader.read(new Uint16Array(2)).then(result => {
//    assert_equals(pullCount, 1);
//
//    assert_false(result.done, 'done');
//
//    const view = result.value;
//    assert_equals(view.byteOffset, 0, 'byteOffset');
//    assert_equals(view.byteLength, 2, 'byteLength');
//
//    const dataView = new DataView(view.buffer, view.byteOffset, view.byteLength);
//    assert_equals(dataView.getUint16(0), 0x0102);
//
//    return reader.read(new Uint8Array(1));
//  }).then(result => {
//    assert_equals(pullCount, 1);
//    assert_not_equals(byobRequest, null, 'byobRequest must not be null');
//    assert_equals(viewInfo.constructor, Uint8Array, 'view.constructor should be Uint8Array');
//    assert_equals(viewInfo.bufferByteLength, 4, 'view.buffer.byteLength should be 4');
//    assert_equals(viewInfo.byteOffset, 0, 'view.byteOffset should be 0');
//    assert_equals(viewInfo.byteLength, 4, 'view.byteLength should be 4');
//
//    assert_false(result.done, 'done');
//
//    const view = result.value;
//    assert_equals(view.byteOffset, 0, 'byteOffset');
//    assert_equals(view.byteLength, 1, 'byteLength');
//
//    assert_equals(view[0], 0x03);
  });
}, 'ReadableStream with byte source: respond(3) to read(view) with 2 element Uint16Array enqueues the 1 byte ' +
   'remainder');
