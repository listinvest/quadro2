"use strict";

let ws;

window.onload = init;

function init() {
    ws = new WebSocket("ws:/" + window.location.hostname + "/ws");
    ws.onmessage = processMessage;
    ws.onclose = reconnect;
    ws.timeout = setTimeout(function() {
        ws.close(); // triggert onclose
    }, 5000);
}

function reconnect() {
    console.error("WebSocket getrennt. Erneut verbinden in 2 s...");
    clearTimeout(ws.timeout);
    ws.timeout = setTimeout(init, 2000);
}

function processMessage(event) {
    let message = event.data;
    let type = message.charAt(0);
    message = message.substr(1);
    let functions = {
        's' : sendStatus,
        'c' : processControl,
        'l' : writeLog,
        'r' : displaySensors
    };
    if (functions.hasOwnProperty(type)) functions[type](message);
    // Timeout
    clearTimeout(ws.timeout);
    ws.timeout = setTimeout(reconnect, 1000);
    // Status anzeigen
    displayConnectivity();
}

function displayConnectivity() {
    if (displayConnectivity.locked == "undefined") {
        displayConnectivity.locked = false;
    }
    if (displayConnectivity.locked) return;
    displayConnectivity.locked = true;
    setTimeout(function(){
        displayConnectivity.locked = false;
    }, 100);
    let e = document.getElementById("ws");
    let spinner = {
        '----------' : '+---------',
        '+---------' : '#+--------',
        '#+--------' : '+#+-------',
        '+#+-------' : '-+#+------',
        '-+#+------' : '--+#+-----',
        '--+#+-----' : '---+#+----',
        '---+#+----' : '----+#+---',
        '----+#+---' : '-----+#+--',
        '-----+#+--' : '------+#+-',
        '------+#+-' : '-------+#+',
        '-------+#+' : '--------+#',
        '--------+#' : '---------+',
        '---------+' : '----------'
    };
    e.innerHTML = spinner[e.innerHTML];
}

function sendStatus(message) {
    ws.send("s1");
}

function processControl(message) {
    ;
}

function writeLog(message) {
    let filter = document.getElementsByName("logFilter")[0].value;
    if (!message.includes(filter)) return;
    let log = document.getElementById("log");
    let level = message.charAt(0);
    let line = document.createElement("pre");
    line.innerHTML = message;
    let colors = {
        'E' : "red",
        'W' : "orange",
        'I' : "green",
        'D' : "black",
        'V' : "gray"
    };
    line.style.color = colors[level];
    log.prepend(line);
}

function displaySensors(message) {
    ;
}

function clearLog() {
    let log = document.getElementById("log");
    while (log.firstChild) {
        log.removeChild(log.firstChild);
    }
}