// license:BSD-3-Clause
// copyright-holders:Grant Galitz, Katelyn Gadd
/***************************************************************************

	JSMAME web audio backend v0.4

	Original by katelyn gadd - kg at luminance dot org ; @antumbral on twitter
	Substantial changes by taisel

***************************************************************************/

// No SharedArrayBuffer requirement: small stereo blocks cross a transferable
// MessagePort, while playback runs on the browser's audio rendering thread.
var jsmame_audio_processor_source = '(' + function () {
 class KaileronAudio extends AudioWorkletProcessor {
  constructor() {
   super();this.ring=new Float32Array(8192);this.read=0;this.write=0;this.count=0;
   this.underruns=0;this.trimmed=0;this.ticks=0;
   this.port.onmessage=event=>{
    if(event.data.clear){this.read=this.write;this.count=0;return;}
    const samples=event.data.samples;if(!(samples instanceof Float32Array))return;
    // Keep at most two 60 Hz frames of queued sound, with room for cadence jitter.
    const maximum=4096;
    const skip=Math.max(0,samples.length-maximum);
    this.trimmed+=skip/2;
    const discard=Math.max(0,this.count+samples.length-skip-maximum);
    this.read=(this.read+discard)%this.ring.length;this.count-=discard;this.trimmed+=discard/2;
    for(let i=skip;i<samples.length;i++){this.ring[this.write]=samples[i];this.write=(this.write+1)%this.ring.length;this.count++;}
   };
  }
  process(inputs,outputs) {
   const channels=outputs[0];if(!channels||!channels[0])return true;
   const frames=channels[0].length;let index=0;
   for(;index<frames&&this.count>=2;index++){
    channels[0][index]=this.ring[this.read];this.read=(this.read+1)%this.ring.length;
    const right=this.ring[this.read];this.read=(this.read+1)%this.ring.length;
    if(channels[1])channels[1][index]=right;this.count-=2;
   }
   if(index<frames)this.underruns++;
   for(;index<frames;index++){channels[0][index]=0;if(channels[1])channels[1][index]=0;}
   if(++this.ticks%8===0)this.port.postMessage({queuedFrames:this.count/2,underruns:this.underruns,trimmedFrames:this.trimmed,blockSize:frames});
   return true;
  }
 }
 registerProcessor('kaileron-audio',KaileronAudio);
}.toString() + ')();';

var jsmame_web_audio = (function () {

var context = null;
var gain_node = null;
var eventNode = null;
var sampleScale = 32766;
var inputBuffer = new Float32Array(44100);
var bufferSize = 44100;
var start = 0;
var rear = 0;
var watchDogDateLast = null;
var watchDogTimerEvent = null;
var blockSize = 1024;
var underruns = 0;
var trimmedFrames = 0;
var initializationFailed = false;
var worklet = null;
var workletStats = null;

function try_worklet() {
 if (!context.audioWorklet || typeof AudioWorkletNode === 'undefined') return;
 var currentContext=context;
 var url=URL.createObjectURL(new Blob([jsmame_audio_processor_source],{type:'application/javascript'}));
 currentContext.audioWorklet.addModule(url).then(function(){
  if(context!==currentContext||context.state==='closed')return;
  var node=new AudioWorkletNode(context,'kaileron-audio',{numberOfInputs:0,numberOfOutputs:1,outputChannelCount:[2]});
  workletStats={queuedFrames:0,underruns:0,trimmedFrames:0,blockSize:128};
  node.port.onmessage=function(event){if(worklet===node)workletStats=event.data;};
  node.onprocessorerror=function(){if(worklet!==node)return;node.disconnect();node.port.onmessage=null;worklet=null;workletStats=null;init_event();};
  disconnect_old_event();worklet=node;node.connect(gain_node);start=rear;
 }).catch(function(){/* The tested ScriptProcessor fallback remains active. */})
 .finally(function(){URL.revokeObjectURL(url);});
}

function lazy_init () {
	//Make
	if (context || initializationFailed) {
		//Return if already created:
		return;
	}
	try {
	if (typeof AudioContext != "undefined") {
		//Standard context creation:
		context = new AudioContext({sampleRate:48000,latencyHint:'interactive'});
	}
	else if (typeof webkitAudioContext != "undefined") {
		//Older webkit context creation:
		context = new webkitAudioContext();
	}
	else {
		//API not found!
		initializationFailed = true;
		return;
	}
	//Generate a volume control node:
	gain_node = context.createGain();
	//Set initial volume to 1:
	gain_node.gain.value = 1.0;
	//Connect volume node to output:
	gain_node.connect(context.destination);
	//Initialize the streaming event:
	init_event();
	try_worklet();
	} catch (error) {
		if (eventNode) {eventNode.onaudioprocess=null;eventNode.disconnect();eventNode=null;}
		if (context && context.close) context.close().catch(function(){});
		context=null;initializationFailed=true;
	}
};

function init_event() {
	//Generate a streaming node point:
	if (typeof context.createScriptProcessor == "function") {
		//Current standard compliant way:
		eventNode = context.createScriptProcessor(blockSize, 0, 2);
	}
	else {
		//Deprecated way:
		eventNode = context.createJavaScriptNode(blockSize, 0, 2);
	}
	//Make our tick function the audio callback function:
	eventNode.onaudioprocess = tick;
	//Connect stream to volume control node:
	eventNode.connect(gain_node);
	//Workarounds for browser issues:
	initializeWatchDog();
};

function initializeWatchDog() {
	watchDogDateLast = (new Date()).getTime();
	if (watchDogTimerEvent === null) {
		watchDogTimerEvent = setInterval(function () {
			var timeDiff = (new Date()).getTime() - watchDogDateLast;
			if (timeDiff > 500 && context && !worklet && context.state === 'running' && !document.hidden) {
				//WORKAROUND FOR FIREFOX BUG:
				//TODO: decide if we want to user agent sniff Firefox here,
				//since Google Chrome doesn't need this:
				disconnect_old_event();
				init_event();
			}
		}, 500);
	}
};

function disconnect_old_event() {
	//Disconnect from audio graph:
	eventNode.disconnect();
	//IIRC there was a firefox bug that did not GC this event when nulling the node itself:
	eventNode.onaudioprocess = null;
	//Null the glitched/unused node:
	eventNode = null;
};

function stream_sink_update (
	pBuffer,           // pointer into emscripten heap. int16 samples
	samples_this_frame // int. number of samples at pBuffer address.
) {
	lazy_init();
	if (!context) return;
	if (samples_this_frame <= 0) return;
	if (worklet) {
		var first=Math.max(0,samples_this_frame-2048),frames=samples_this_frame-first;
		var samples=new Float32Array(frames*2);
		for(var frame=0;frame<frames;frame++){
			var at=(pBuffer/2)+(first+frame)*2;
			samples[frame*2]=HEAP16[at]/sampleScale;samples[frame*2+1]=HEAP16[at+1]/sampleScale;
		}
		worklet.port.postMessage({samples:samples},[samples.buffer]);return;
	}

	for (
		var i = 0,
		l = samples_this_frame | 0;
		i < l;
		i++
	) {
		var offset =
			// divide by sizeof(int16_t) since pBuffer is offset
			//  in bytes
			((pBuffer / 2) | 0) +
			((i * 2) | 0);

		var left_sample = HEAP16[offset];
		var right_sample = HEAP16[(offset + 1) | 0];

		// normalize from signed int16 to signed float
		var left_sample_float = left_sample / sampleScale;
		var right_sample_float = right_sample / sampleScale;

		inputBuffer[rear++] = left_sample_float;
		inputBuffer[rear++] = right_sample_float;
		if (rear == bufferSize) {
			rear = 0;
		}
		if (start == rear) {
			start += 2;
			if (start == bufferSize) {
				start = 0;
			}
		}
	}
	// Catch-up/suspension must not build a half-second tail of stale audio.
	var queued = sample_count();
	var maximum = blockSize * 8;
	if (queued > maximum) {
		trimmedFrames += (queued - maximum) / 2;
		start = (rear - maximum + bufferSize) % bufferSize;
	}
};

function tick (event) {
	//Find all output channels:
	for (var bufferCount = 0, buffers = []; bufferCount < 2; ++bufferCount) {
		buffers[bufferCount] = event.outputBuffer.getChannelData(bufferCount);
	}
	//Copy samples from the input buffer to the Web Audio API:
	for (var index = 0; index < buffers[0].length && start != rear; ++index) {
		buffers[0][index] = inputBuffer[start++];
		buffers[1][index] = inputBuffer[start++];
		if (start == bufferSize) {
			start = 0;
		}
	}
	if (index < buffers[0].length) underruns++;
	// Silence is finite even when no sample was produced. The old empty-buffer
	// path read outside the output array and generated NaNs.
	while (index < buffers[0].length) {buffers[0][index] = 0;buffers[1][index++] = 0;}
	//Deep inside the bowels of vendors bugs,
	//we're using watchdog for a firefox bug,
	//where the user agent decides to stop firing events
	//if the user agent lags out due to system load.
	//Don't even ask....
	watchDogDateLast = (new Date()).getTime();
}

function get_context() {
	return context;
};

function sample_count() {
	//TODO get someone to call this from the emulator,
	//so the emulator can do proper audio buffering by
	//knowing how many samples are left:
	if (!context) {
		//Use impossible value as an error code:
		return -1;
	}
	if (worklet) return workletStats ? workletStats.queuedFrames*2 : 0;
	var count = rear - start;
	if (start > rear) {
		count += bufferSize;
	}
	return count;
}

return {
	stream_sink_update: stream_sink_update,
	get_context: get_context,
	sample_count: sample_count,
	clear: function(){start=rear;if(worklet){worklet.port.postMessage({clear:true});workletStats.queuedFrames=0;}},
	stats: function(){return {version:3,state:context?context.state:'unavailable',backend:worklet?'audio-worklet':'script-processor',bufferMs:context?Math.max(0,sample_count())*500/context.sampleRate:0,blockSize:workletStats?workletStats.blockSize:blockSize,underruns:workletStats?workletStats.underruns:underruns,trimmedFrames:workletStats?workletStats.trimmedFrames:trimmedFrames,baseLatencyMs:context?(context.baseLatency||0)*1000:0,outputLatencyMs:context?(context.outputLatency||0)*1000:0};},
	resume: function(){
		if (!context || context.state === 'closed') {if(worklet){worklet.disconnect();worklet.port.onmessage=null;}worklet=null;workletStats=null;context=null;initializationFailed=false;start=rear;lazy_init();}
		return context?context.resume():Promise.reject(new Error('게임 오디오 장치를 사용할 수 없습니다.'));
	}
};

})();

window.jsmame_stream_sink_update = jsmame_web_audio.stream_sink_update;
window.jsmame_sample_count = jsmame_web_audio.sample_count;
window.jsmame_audio_status = jsmame_web_audio.stats;
window.jsmame_audio_clear = jsmame_web_audio.clear;
window.jsmame_audio_resume = jsmame_web_audio.resume;
window.jsmame_audio_suspend = function(){jsmame_web_audio.clear();var context=jsmame_web_audio.get_context();return context?context.suspend():Promise.resolve();};
