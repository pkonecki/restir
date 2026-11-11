// Emscripten's emdawnwebgpu port forwards WGPU_QUERY_SET_INDEX_UNDEFINED
// (0xFFFFFFFF) in GPU pass timestamp writes verbatim, where the browser
// expects the member to be left out. bgfx's GPU timer writes only the start
// of each pass, so every command encoder holding a timer query would fail
// validation. Drop the unset indices before they reach the browser.
if (typeof GPUCommandEncoder !== 'undefined') {
	const kQuerySetIndexUndefined = 0xFFFFFFFF;
	const fixTimestampWrites = (descriptor) => {
		const writes = descriptor && descriptor.timestampWrites;
		if (writes) {
			if (writes.beginningOfPassWriteIndex === kQuerySetIndexUndefined) delete writes.beginningOfPassWriteIndex;
			if (writes.endOfPassWriteIndex === kQuerySetIndexUndefined) delete writes.endOfPassWriteIndex;
		}
		return descriptor;
	};
	for (const name of ['beginComputePass', 'beginRenderPass']) {
		const original = GPUCommandEncoder.prototype[name];
		GPUCommandEncoder.prototype[name] = function (descriptor) {
			return original.call(this, fixTimestampWrites(descriptor));
		};
	}
}
