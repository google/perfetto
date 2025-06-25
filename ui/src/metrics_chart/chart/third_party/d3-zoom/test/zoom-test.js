import assert from 'assert';
import {select} from 'd3-selection';
import {
  zoom,
  zoomIdentity,
  zoomTransform,
  ZoomTransform,
} from '../src/index.js';
import it from './jsdom.js';

it('zoom initiates a zooming behavior', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(1, 0, 0));
  div.call(z.transform, zoomIdentity.scale(2).translate(1, -3));
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(2, 2, -6));
});

it('zoomTransform returns the node’s current transform', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  assert.deepStrictEqual(zoomTransform(div.node()), new ZoomTransform(1, 0, 0));
  div.call(z.translateBy, 10, 10);
  assert.deepStrictEqual(
      zoomTransform(div.node()),
      new ZoomTransform(1, 10, 10),
  );
  assert.deepStrictEqual(
      zoomTransform(div.append('span').node()),
      new ZoomTransform(1, 10, 10),
  ); // or an ancestor's…
  // or zoomIdentity
  assert.deepStrictEqual(zoomTransform(document.body), zoomIdentity);
});

it('zoom.scaleBy zooms', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  div.call(z.scaleBy, 2, [0, 0]);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(2, 0, 0));
  div.call(z.scaleBy, 2, [2, -2]);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(4, -2, 2));
  div.call(z.scaleBy, 1 / 4, [2, -2]);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(1, 1, -1));
});

it('zoom.scaleTo zooms', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  div.call(z.scaleTo, 2);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(2, 0, 0));
  div.call(z.scaleTo, 2);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(2, 0, 0));
  div.call(z.scaleTo, 1);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(1, 0, 0));
});

it('zoom.translateBy translates', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  div.call(z.translateBy, 10, 10);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(1, 10, 10));
  div.call(z.scaleBy, 2);
  div.call(z.translateBy, -10, -10);
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(2, 0, 0));
});

it('zoom.scaleBy arguments can be functions passed (datum, index)', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  let a; let b; let c; let d;
  div.call(
      z.scaleBy,
      function() {
        // eslint-disable-next-line prefer-rest-params
        a = arguments;
        // eslint-disable-next-line no-invalid-this
        b = this;
        return 2;
      },
      function() {
        // eslint-disable-next-line prefer-rest-params
        c = arguments;
        // eslint-disable-next-line no-invalid-this
        d = this;
        return [0, 0];
      },
  );
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(2, 0, 0));
  assert.deepStrictEqual(a[0], 'hello');
  assert.deepStrictEqual(a[1], 0);
  assert.deepStrictEqual(b, div.node());
  assert.deepStrictEqual(c[0], 'hello');
  assert.deepStrictEqual(c[1], 0);
  assert.deepStrictEqual(d, div.node());
});

it('zoom.scaleTo arguments can be functions passed (datum, index)', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  let a; let b; let c; let d;
  div.call(
      z.scaleTo,
      function() {
        // eslint-disable-next-line prefer-rest-params
        a = arguments;
        // eslint-disable-next-line no-invalid-this
        b = this;
        return 2;
      },
      function() {
        // eslint-disable-next-line prefer-rest-params
        c = arguments;
        // eslint-disable-next-line no-invalid-this
        d = this;
        return [0, 0];
      },
  );
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(2, 0, 0));
  assert.deepStrictEqual(a[0], 'hello');
  assert.deepStrictEqual(a[1], 0);
  assert.deepStrictEqual(b, div.node());
  assert.deepStrictEqual(c[0], 'hello');
  assert.deepStrictEqual(c[1], 0);
  assert.deepStrictEqual(d, div.node());
});

it('zoom.translateBy arguments can be functions passed (datum, index)', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  let a; let b; let c; let d;
  div.call(
      z.translateBy,
      function() {
        // eslint-disable-next-line prefer-rest-params
        a = arguments;
        // eslint-disable-next-line no-invalid-this
        b = this;
        return 2;
      },
      function() {
      // eslint-disable-next-line prefer-rest-params
        c = arguments;
        // eslint-disable-next-line no-invalid-this
        d = this;
        return 3;
      },
  );
  assert.deepStrictEqual(div.node().__zoom, new ZoomTransform(1, 2, 3));
  assert.deepStrictEqual(a[0], 'hello');
  assert.deepStrictEqual(a[1], 0);
  assert.deepStrictEqual(b, div.node());
  assert.deepStrictEqual(c[0], 'hello');
  assert.deepStrictEqual(c[1], 0);
  assert.deepStrictEqual(d, div.node());
});

it('zoom.constrain receives (transform, extent, translateExtent)', () => {
  const div = select(document.body).append('div').datum('hello');
  const z = zoom();
  div.call(z);
  const constrain = z.constrain();
  let a;
  let b;
  z.constrain(function() {
    // eslint-disable-next-line prefer-rest-params
    a = arguments;
    // eslint-disable-next-line no-invalid-this, prefer-rest-params
    return (b = constrain.apply(this, arguments));
  });
  div.call(z.translateBy, 10, 10);
  assert.deepStrictEqual(a[0], b);
  assert.deepStrictEqual(a[0], new ZoomTransform(1, 10, 10));
  assert.deepStrictEqual(a[1], [
    [0, 0],
    [0, 0],
  ]);
  assert.strictEqual(a[2][0][0], -Infinity);
  z.constrain(constrain);
});
