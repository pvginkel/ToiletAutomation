// The dedicated worker that holds the port. The page only asks for the port and shows status.
import { createEngine } from './engine.js';

const engine = createEngine({
  serial: self.navigator.serial,
  context: 'worker',
  toPage: (msg) => self.postMessage(msg),
});
self.onmessage = (e) => engine.fromPage(e.data);
self.postMessage({ t: 'ready', serialInWorker: !!self.navigator.serial });
