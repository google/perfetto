import {dispatch, select, pointer, interrupt} from 'd3';
import constant from './constant.js';
import ZoomEvent from './event.js';
import {Transform, identity} from './transform.js';
import noevent, {nopropagation} from './noevent.js';

// Ignore right-click, since that should open the context menu.
// except for pinch-to-zoom, which is sent as a wheel+ctrlKey event
function defaultFilter(event) {
  return (!event.ctrlKey || event.type === 'wheel') && !event.button;
}

function defaultExtent() {
  // eslint-disable-next-line no-invalid-this
  let e = this;
  if (e instanceof SVGElement) {
    e = e.ownerSVGElement || e;
    if (e.hasAttribute('viewBox')) {
      e = e.viewBox.baseVal;
      return [
        [e.x, e.y],
        [e.x + e.width, e.y + e.height],
      ];
    }
    return [
      [0, 0],
      [e.width.baseVal.value, e.height.baseVal.value],
    ];
  }
  return [
    [0, 0],
    [e.clientWidth, e.clientHeight],
  ];
}

function defaultTransform() {
  // eslint-disable-next-line no-invalid-this
  return this.__zoom || identity;
}

function defaultWheelDelta(event) {
  return (
    -event.deltaY *
    (event.deltaMode === 1 ? 0.05 : event.deltaMode ? 1 : 0.002) *
    (event.ctrlKey ? 10 : 1)
  );
}

function defaultTouchable() {
  // eslint-disable-next-line no-invalid-this
  return navigator.maxTouchPoints || 'ontouchstart' in this;
}

function defaultConstrain(transform, extent, translateExtent) {
  const dx0 = transform.invertX(extent[0][0]) - translateExtent[0][0];
  const dx1 = transform.invertX(extent[1][0]) - translateExtent[1][0];
  const dy0 = transform.invertY(extent[0][1]) - translateExtent[0][1];
  const dy1 = transform.invertY(extent[1][1]) - translateExtent[1][1];
  return transform.translate(
    dx1 > dx0 ? (dx0 + dx1) / 2 : Math.min(0, dx0) || Math.max(0, dx1),
    dy1 > dy0 ? (dy0 + dy1) / 2 : Math.min(0, dy0) || Math.max(0, dy1),
  );
}

export default function zoom() {
  const filter = defaultFilter;
  let extent = defaultExtent;
  const constrain = defaultConstrain;
  const wheelDelta = defaultWheelDelta;
  const touchable = defaultTouchable;
  const scaleExtent = [0, Infinity];
  const translateExtent = [
    [-Infinity, -Infinity],
    [Infinity, Infinity],
  ];
  const listeners = dispatch('start', 'zoom', 'end');
  let touchstarting;
  let touchfirst;
  let touchending;
  const touchDelay = 500;
  const wheelDelay = 150;
  const tapDistance = 10;

  function Gesture(that, args) {
    this.that = that;
    this.args = args;
    this.active = 0;
    this.sourceEvent = null;
    this.extent = extent.apply(that, args);
    this.taps = 0;
  }

  Gesture.prototype = {
    event: function(event) {
      if (event) {
        this.sourceEvent = event;
      }
      return this;
    },
    start: function() {
      if (++this.active === 1) {
        this.that.__zooming = this;
        this.emit('start');
      }
      return this;
    },
    zoom: function(key, transform) {
      if (this.mouse && key !== 'mouse') {
        this.mouse[1] = transform.invert(this.mouse[0]);
      }
      if (this.touch0 && key !== 'touch') {
        this.touch0[1] = transform.invert(this.touch0[0]);
      }
      if (this.touch1 && key !== 'touch') {
        this.touch1[1] = transform.invert(this.touch1[0]);
      }
      this.that.__zoom = transform;
      this.emit('zoom');
      return this;
    },
    end: function() {
      if (--this.active === 0) {
        delete this.that.__zooming;
        this.emit('end');
      }
      return this;
    },
    emit: function(type) {
      const d = select(this.that).datum();
      listeners.call(
          type,
          this.that,
          new ZoomEvent(type, {
            sourceEvent: this.sourceEvent,
            target: _zoom,
            type,
            transform: this.that.__zoom,
            dispatch: listeners,
          }),
          d,
      );
    },
  };

  function scale(transform, k) {
    k = Math.max(scaleExtent[0], Math.min(scaleExtent[1], k));
    return k === transform.k ? transform : new Transform(k, transform.x, 0);
  }

  function translate(transform, p0, p1) {
    const x = p0[0] - p1[0] * transform.k;
    return x === transform.x ? transform : new Transform(transform.k, x, 0);
  }

  function gesture(that, args, clean) {
    return (!clean && that.__zooming) || new Gesture(that, args);
  }

  let isScrolling;
  let initialG;

  function wheeled(event, ...args) {
    // eslint-disable-next-line no-invalid-this, prefer-rest-params
    if (!filter.apply(this, arguments)) {
      return;
    }

    let isPan = false;

    // pan
    if (Math.abs(event.deltaX) >= Math.abs(event.deltaY)) {
      isPan = true;
    } else if (!event.ctrlKey) {
      // scroll
      return;
    }

    noevent(event);
    // eslint-disable-next-line no-invalid-this
    const g = gesture(this, args).event(event);
    // eslint-disable-next-line no-invalid-this
    const t = this.__zoom;
    const k = Math.max(
        scaleExtent[0],
        Math.min(
            scaleExtent[1],
            // eslint-disable-next-line no-invalid-this, prefer-rest-params
            t.k * Math.pow(2, wheelDelta.apply(this, arguments)),
        ),
    );
    const p = pointer(event);
    function wheelidled() {
      g.wheel = null;
      g.end();
    }

    // If the mouse is in the same location as before, reuse it.
    // If there were recent wheel events, reset the wheel idle timeout.
    if (g.wheel) {
      if (g.mouse[0][0] !== p[0] || g.mouse[0][1] !== p[1]) {
        g.mouse[1] = t.invert((g.mouse[0] = p));
      }
      clearTimeout(g.wheel);
    } else if (!isPan && t.k === k) {
      // If this wheel event won’t trigger a transform change, ignore it.
      return;
    } else {
      // Otherwise, capture the mouse point and location at the start.
      g.mouse = [p, t.invert(p)];
      // eslint-disable-next-line no-invalid-this
      interrupt(this);
      g.start();
    }

    g.wheel = setTimeout(wheelidled, wheelDelay);

    if (isPan) {
      if (!isScrolling) {
        initialG = g;
      } else {
        initialG
            .event(event)
            .zoom(
                'mouse',
                constrain(
                    translate(
                        initialG.that.__zoom,
                        (initialG.mouse[0] = [
                          initialG.mouse[0][0] - event.deltaX,
                          initialG.mouse[0][1],
                        ]),
                        initialG.mouse[1],
                    ),
                    initialG.extent,
                    translateExtent,
                ),
            );
      }
    } else {
      g.zoom(
          'mouse',
          constrain(
              translate(scale(t, k), g.mouse[0], g.mouse[1]),
              g.extent,
              translateExtent,
          ),
      );
    }

    if (isPan) {
      clearTimeout(isScrolling);
      isScrolling = setTimeout(function() {
        isScrolling = null;
      }, 66);
    }
  }

  let currentMouseMoveEvent;

  function keydowned(event, target, ...args) {
    if (
      // eslint-disable-next-line prefer-rest-params
      !filter.apply(target, arguments) ||
      event.target.tagName === 'INPUT' ||
      event.target.tagName === 'TEXTAREA'
    ) {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    let deltaX;
    let deltaY;

    switch (event.key) {
      case 'w':
      case 'W':
        deltaY = -5;
        break;
      case 's':
      case 'S':
        deltaY = 5;
        break;
      case 'a':
      case 'A':
        deltaX = -50;
        break;
      case 'd':
      case 'D':
        deltaX = 50;
        break;
      default:
        break;
    }

    const g = gesture(target, args).event(event);
    const t = target.__zoom;
    if (deltaX) {
      interrupt(target);
      g.start();
      g.zoom(
          'key',
          constrain(
              new Transform(t.k, t.x - deltaX, t.y),
              g.extent,
              translateExtent,
          ),
      );
      g.end();
    } else if (deltaY && currentMouseMoveEvent) {
      const p = pointer(currentMouseMoveEvent, target);
      const k = Math.max(
          scaleExtent[0],
          Math.min(
              scaleExtent[1],
              t.k *
            Math.pow(
                2,
                wheelDelta.apply(target, [
                  {
                    deltaY,
                    deltaMode: 0,
                    ctrlKey: true,
                  },
                ]),
            ),
          ),
      );

      if (t.k === k) {
        return;
      } else {
        g.key = [p, t.invert(p)];
        interrupt(target);
        g.start();
        g.zoom(
            'key',
            constrain(
                translate(scale(t, k), g.key[0], g.key[1]),
                g.extent,
                translateExtent,
            ),
        );
        g.end();
      }
    }
  }

  function mousemoved(event) {
    currentMouseMoveEvent = event;
  }

  function touchstarted(event, ...args) {
    // eslint-disable-next-line no-invalid-this, prefer-rest-params
    if (!filter.apply(this, arguments)) {
      return;
    }
    const touches = event.touches;
    const n = touches.length;
    // eslint-disable-next-line no-invalid-this
    const g = gesture(this, args, event.changedTouches.length === n)
        .event(event);
    let started;
    let i;
    let t;
    let p;

    nopropagation(event);
    for (i = 0; i < n; ++i) {
      // eslint-disable-next-line no-invalid-this
      (t = touches[i]), (p = pointer(t, this));
      // eslint-disable-next-line no-invalid-this
      p = [p, this.__zoom.invert(p), t.identifier];
      if (!g.touch0) {
        g.touch0 = p;
        started = true;
        g.taps = 1 + !!touchstarting;
      } else if (!g.touch1 && g.touch0[2] !== p[2]) {
        g.touch1 = p;
        g.taps = 0;
      }
    }

    if (touchstarting) {
      touchstarting = clearTimeout(touchstarting);
    }

    if (started) {
      if (g.taps < 2) {
        (touchfirst = p[0]),
        (touchstarting = setTimeout(function() {
          touchstarting = null;
        }, touchDelay));
      }
      // eslint-disable-next-line no-invalid-this
      interrupt(this);
      g.start();
    }
  }

  function touchmoved(event, ...args) {
    // eslint-disable-next-line no-invalid-this
    if (!this.__zooming) {
      return;
    }
    // eslint-disable-next-line no-invalid-this
    const g = gesture(this, args).event(event);
    const touches = event.changedTouches;
    const n = touches.length;
    let i;
    let t;
    let p;
    let l;

    noevent(event);
    for (i = 0; i < n; ++i) {
      // eslint-disable-next-line no-invalid-this
      (t = touches[i]), (p = pointer(t, this));
      if (g.touch0 && g.touch0[2] === t.identifier) {
        g.touch0[0] = p;
      } else if (g.touch1 && g.touch1[2] === t.identifier) {
        g.touch1[0] = p;
      }
    }
    t = g.that.__zoom;
    if (g.touch1) {
      const p0 = g.touch0[0];
      const l0 = g.touch0[1];
      const p1 = g.touch1[0];
      const l1 = g.touch1[1];
      dp = (dp = p1[0] - p0[0]) * dp + (dp = p1[1] - p0[1]) * dp;
      dl = (dl = l1[0] - l0[0]) * dl + (dl = l1[1] - l0[1]) * dl;
      t = scale(t, Math.sqrt(dp / dl));
      p = [(p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2];
      l = [(l0[0] + l1[0]) / 2, (l0[1] + l1[1]) / 2];
    } else if (g.touch0) {
      p = g.touch0[0];
      l = g.touch0[1];
    } else {
      return;
    }

    g.zoom('touch', constrain(translate(t, p, l), g.extent, translateExtent));
  }

  function touchended(event, ...args) {
    // eslint-disable-next-line no-invalid-this
    if (!this.__zooming) {
      return;
    }
    // eslint-disable-next-line no-invalid-this
    const g = gesture(this, args).event(event);
    const touches = event.changedTouches;
    const n = touches.length;
    let i;
    let t;

    nopropagation(event);
    if (touchending) {
      clearTimeout(touchending);
    }
    touchending = setTimeout(function() {
      touchending = null;
    }, touchDelay);
    for (i = 0; i < n; ++i) {
      t = touches[i];
      if (g.touch0 && g.touch0[2] === t.identifier) {
        delete g.touch0;
      } else if (g.touch1 && g.touch1[2] === t.identifier) {
        delete g.touch1;
      }
    }
    if (g.touch1 && !g.touch0) {
      g.touch0 = g.touch1;
      delete g.touch1;
    }
    if (g.touch0) {
      // eslint-disable-next-line no-invalid-this
      g.touch0[1] = this.__zoom.invert(g.touch0[0]);
    } else {
      g.end();
      // If this was a dbltap, reroute to the (optional) dblclick.zoom handler.
      if (g.taps === 2) {
        // eslint-disable-next-line no-invalid-this
        t = pointer(t, this);
        if (
          Math.hypot(touchfirst[0] - t[0], touchfirst[1] - t[1]) < tapDistance
        ) {
          // eslint-disable-next-line no-invalid-this
          const p = select(this).on('dblclick.zoom');
          if (p) {
            // eslint-disable-next-line no-invalid-this, prefer-rest-params
            p.apply(this, arguments);
          }
        }
      }
    }
  }

  function _zoom(selection) {
    selection
        .property('__zoom', defaultTransform)
        .on('wheel.zoom', wheeled, {passive: false})
        .on('mousemove.zoom', mousemoved)
        .filter(touchable)
        .on('touchstart.zoom', touchstarted)
        .on('touchmove.zoom', touchmoved)
        .on('touchend.zoom touchcancel.zoom', touchended)
        .style('-webkit-tap-highlight-color', 'rgba(0,0,0,0)');

    select(selection.node()).on('keydown.zoom', (event, ...args) => {
      keydowned(event, selection.node(), ...args);
    });
  }

  _zoom.extent = function(_) {
    return (
      (extent =
        typeof _ === 'function' ?
        _ : constant([
          [+_[0][0], +_[0][1]],
          [+_[1][0], +_[1][1]],
        ])),
      _zoom
    );
  };

  _zoom.scaleExtent = function(_) {
    return (scaleExtent[0] = +_[0]), (scaleExtent[1] = +_[1]), _zoom;
  };

  _zoom.on = function() {
    // eslint-disable-next-line prefer-spread, prefer-rest-params
    listeners.on.apply(listeners, arguments);
    return _zoom;
  };

  return _zoom;
}
