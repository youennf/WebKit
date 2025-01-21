// META: title=Service Worker: navigator.serviceWorker.ready
// META: script=/service-workers/service-worker/resources/test-helpers.sub.js

promise_test(async t => {
  const url = 'resources/empty-worker.js';
  const scope = 'resources/blank.html?ready-controlled';
  const expectedURL = normalizeURL(url);
  const registration = await service_worker_unregister_and_register(t, url, scope);
  t.add_cleanup(() => registration.unregister());

  await wait_for_state(t, registration.installing, 'activated');

  const frame = await with_iframe(scope);
  t.add_cleanup(() => frame.remove());

  const readyReg = await frame.contentWindow.navigator.serviceWorker.ready;

  assert_equals(readyReg.installing, null, 'installing should be null');
  assert_equals(readyReg.waiting, null, 'waiting should be null');
  assert_equals(readyReg.active.scriptURL, expectedURL, 'active after ready should not be null');
  assert_equals(
    frame.contentWindow.navigator.serviceWorker.controller,
    readyReg.active,
    'the controller should be the active worker'
  );
  assert_in_array(
    readyReg.active.state,
    ['activating', 'activated'],
    '.ready should be resolved when the registration has an active worker'
  );
}, 'ready on a controlled document');
