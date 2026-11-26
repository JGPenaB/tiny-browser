order.push('tla start');
await new Promise(resolve => window.addEventListener('load', resolve));
order.push('tla end');
export const done = true;
