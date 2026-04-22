//port is usb connection to pico 2
//reader = incominf stream of data
//write is outgoing stream from website to pico 2
let port, reader, writer; //store active connection to pico and streams
//initialise web audio api
//manage audio data and playback in browser.
const audioCtx = new (window.AudioContext || window.webkitAudioContext)();
let audioBufferData = [];

//DOM Elements
//to link html and javascript elements.

//displaying elements
const noteEl = document.getElementById('note-display');
const hzEl = document.getElementById('hz-display');
const stateEl = document.getElementById('state_indicator');
//buttons
const recordBtn = document.getElementById('record-btn');
const connectBtn = document.getElementById('connect-btn');
//display spectrum
const spectrumArea = document.getElementById('spectrum-area');
const canvas = document.getElementById('spectrum-canvas');
const ctx = canvas.getContext('2d');
//interactive
const replayBtn = document.getElementById('replay-btn');
const pitchSlider = document.getElementById('pitch-slider');
const pitchLabel = document.getElementById('pitch-label');
//function to switch between pages. does it with a loop
function showPage(pageId) {
    document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
    document.getElementById(pageId).classList.add('active');
}
//listener for pitch slider and updates immediately when changed by  user.
pitchSlider.oninput = () => {
    pitchLabel.innerText = `Current: ${pitchSlider.value}x ${pitchSlider.value > 2 ? "(High Pitch!)" : ""}`;
};
//replay the audio that was recorded + manipulated.
replayBtn.onclick = async () => {
    if (!writer) return;
    const pitch = parseFloat(pitchSlider.value).toFixed(1);
    await writer.write(`P${pitch}\n`);
};
connectBtn.addEventListener('click', async () => {
    try {
        //request permission from user for serial port
        port = await navigator.serial.requestPort();
        //baud rate is default pico 2 setting.
        await port.open({ baudRate: 115200 });
        //when connected it becomes green and conenct button set to none to disappear
        document.getElementById('status').innerText = "🟢 Connected";
        connectBtn.style.display = "none";
        //encoders to send text, decoders to receive/read.
        const encoder = new TextEncoderStream();
        encoder.readable.pipeTo(port.writable);
        writer = encoder.writable.getWriter();
        const decoder = new TextDecoderStream();
        port.readable.pipeTo(decoder.writable);
        //line break transformer is used to split incoming data into lines for easier processing.
        const inputStream = decoder.readable.pipeThrough(new TransformStream(new LineBreakTransformer()));
        reader = inputStream.getReader();
        readLoop();
    } catch (e) { alert("Connection failed."); }
});
recordBtn.addEventListener('click', async () => {
    if (!writer) return alert("Connect Pico first!");
    recordBtn.disabled = true;
    audioBufferData = []; 
    replayBtn.disabled = true;
    spectrumArea.style.display = "none";
    //3 sec countdown to give user time to prepare. ux consideration.
    let countdown = 3;
    const timer = setInterval(() => {
        stateEl.innerText = `Starting in ${countdown}...`;
        if (countdown-- === 0) {
            clearInterval(timer);
            startRecording();
        }
    }, 1000);
});
async function startRecording() {
    //sends R to pico 2 so it can start recording
    //(found in main loop of c++ code)
    await writer.write("R");
    stateEl.innerText = "Recording...";
    setTimeout(() => {
        //disables record button bc it is unnecessary during recording process.
        recordBtn.disabled = false;
        stateEl.innerText = "Ready";
    }, 4500); 
}
async function readLoop() {
    while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        //verify the type of incoming data and process accordingly.
        //wav for audio replay
        //data to update note + frequency display
        //spec for frequency spectrum display
        if (value.startsWith("WAV:")) {
            //extract raw audio data from incoming string, convert to numbers and store in audioBufferData array.
            const raw = value.replace("WAV:", "").split(",").map(Number);
            audioBufferData.push(...raw);
            replayBtn.disabled = false;
            
        } else if (value.startsWith("DATA:")) {
            //extract note and frequency from incoming string. update display elements accordingly. if note is silent, hide spectrum area.
            replayBtn.disabled = false;
            const payload = value.replace("DATA:", "").split("|");
            if (payload[0] !== "Silent") {
                noteEl.innerText = payload[0];
                hzEl.innerText = payload[1] + " Hz";
                spectrumArea.style.display = "flex";
            }
            
        } else if (value.startsWith("SPEC:")) {
            const bins = value.replace("SPEC:", "").split(",").map(Number);
            drawSpectrum(bins);
        }
    }
}
//clearrect to clear previous spectrum to draw new when a new recording occurs
function drawSpectrum(bins) {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    //calculate bar width and height based on bin values. normalise to fit canvas.
    const barWidth = canvas.width / bins.length;
    const maxBin = Math.max(...bins) || 1; 
    bins.forEach((bin, i) => {
        const barHeight = (bin / maxBin) * (canvas.height - 10);
        ctx.fillStyle = '#3498db';  //colour of bars
        //draw new bars
        ctx.fillRect(i * barWidth, canvas.height - barHeight, barWidth - 1, barHeight);
    });
}
//transform stream to split incoming data into lines for easier processing.
//serial data often comes sliced. this acts as a buffer to ensure we get complete lines of data before processing.
class LineBreakTransformer {
    constructor() { this.container = ""; }
    transform(chunk, controller) {
        this.container += chunk;
        const lines = this.container.split("\n");
        this.container = lines.pop();
        lines.forEach(line => controller.enqueue(line));}
    flush(controller) { controller.enqueue(this.container); }
}