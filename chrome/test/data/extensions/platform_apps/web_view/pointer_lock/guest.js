// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// See chrome/browser/extensions/web_view_interactive_browsertest.cc
// (WebViewInteractiveTest, PointerLock) for documentation on this test.

function LockMouse(element) {
  element.requestPointerLock = element.webkitRequestPointerLock;
  element.requestPointerLock();
}
var first_lock = true;
document.onwebkitpointerlockchange = function () {
  if (document.webkitPointerLockElement) {
    if (first_lock) {
      console.log('locked');
      setTimeout(function () { embedder.postMessage("locked", "*"); }, 500);
    } else {
      console.log('deleting...');
      setTimeout(function () { embedder.postMessage("delete me", "*"); }, 500);
    }
    first_lock = false;
  } else {
    console.log('unlocked');
    embedder.postMessage('unlocked', "*");
  }
}

var embedder = null;
window.addEventListener("message", function(e) {
  embedder = e.source;
  embedder.postMessage('connected', '*');
});

document.getElementById('locktarget1').addEventListener("mousemove",
    function (e) {
  setTimeout(function () { embedder.postMessage("mouse-move", "*"); }, 500);
  if (info.innerHTML != "fail")
    info.innerHTML = "Info: movementX: "+ e.webkitMovementX +
        ", movementY: " + e.webkitMovementY;
});

document.getElementById('locktarget2').addEventListener("mousemove",
    function (e) {
  info.innerHTML = "fail";
  embedder.postMessage('Pointer was not locked to locktarget1.', "*");
});

document.getElementById('button1').addEventListener("click", function (e) {
  console.log("click captured, locking mouse");
  LockMouse(locktarget1);
}, false);

document.getElementById('button2').addEventListener("click", function (e) {
  console.log('clicked button 2');
  embedder.postMessage('clicked', "*");
}, false);

document.onkeydown = function (e) {
  console.log("key handled, unlocking");
  document.webkitExitPointerLock();
};

