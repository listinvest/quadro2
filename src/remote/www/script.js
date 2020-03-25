"use strict";

NodeList.prototype.indexOf = Array.prototype.indexOf;
let $ = (s, o = document) => o.querySelectorAll(s);

function empty(e) {
    while (e.firstChild) {
        e.removeChild(e.firstChild);
    }
}

let ws;

window.onload = init;

function init() {
    ws = new WebSocket("ws:/" + window.location.hostname + "/ws");
    ws.onmessage = processMessage;
    ws.onclose = reconnect;
    // ws.timeout = setTimeout(function() {
    //     ws.close(); // triggert onclose
    // }, 5000);
}

function reconnect() {
    console.error("WebSocket getrennt. Erneut verbinden in 2 s...");
    clearTimeout(ws.timeout);
    ws.timeout = setTimeout(init, 2000);
}

function processMessage(event) {
    try {
        let json = JSON.parse(event.data);
        let functions = [
            undefined,
            gotStatus,
            gotLog,
            undefined,
            settingResponse,
            gotParameter,
            gotPv,
            gotCommandList,
            gotSettingList,
            gotParameterList,
            gotPvList
        ];
        functions[json[0]](json[1]);
    } catch (e) {
        console.error(e);
    }
    // clearTimeout(ws.timeout);
    // ws.timeout = setTimeout(reconnect, 2000);
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
    let e = $("#ws")[0];
    let spinner = {
        '-' : '\\',
        '\\' : '|',
        '|' : '/',
        '/' : '-',
    };
    e.innerHTML = spinner[e.innerHTML];
}

function gotStatus(message) {
    ws.send("[1,1]");
}

function gotLog(message) {
    let filter = $("[name=logFilter]")[0].value;
    if (!message.includes(filter)) return;
    let log = $("#log")[0];
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

function gotParameter(parameter) {
    console.log(parameter);
    return;
}

function gotCommandList(commands) {
    let f = $("#commands")[0];
    empty(f);
    for (let node of commands) {
        let set = document.createElement("fieldset");
        set.name = node[0];
        set.innerHTML = "<legend>" + node[0] + "</legend>";
        for (let element of node[1]) {
            set.innerHTML += "<input type='button' value='" + element + "' name='" + element + "'> ";
        }
        f.appendChild(set);
    }
    f.addEventListener("click", commandClick, true);
}

function commandClick(event) {
    let t = event.target;
    let p = t.parentNode;
    let n = $("fieldset", t.form).indexOf(p);
    let e = $("input", p).indexOf(t);
    ws.send("[3,[" + n + "," + e + "]]");
}

function gotSettingList(settings) {
    let f = $("#settings")[0];
    empty(f);
    for (let node of settings) {
        let set = document.createElement("fieldset");
        set.name = node[0];
        set.innerHTML = "<legend>" + node[0] + "</legend>";
        for (let element of node[1]) {
            set.innerHTML += "<input type='number' name='" + element + "'> ";
            set.innerHTML += "<label for='" + element + "'>" + element + "</label><br>";
        }
        f.appendChild(set);
    }
    $("input", f).forEach(settingRequest);
    f.addEventListener("blur", settingBlur, true);
}

function settingRequest(i) {
    let p = i.parentNode;
    let n = $("fieldset", i.form).indexOf(p);
    let e = $("input", p).indexOf(i);
    ws.send("[4,[" + n + "," + e + "]]");
}

function settingResponse(setting) {
    let f = $("#settings")[0];
    let n = $("fieldset", f)[setting[0]];
    let e = $("input", n)[setting[1]];
    e.setAttribute("qType", setting[2]);
    e.value = setting[3];
}

function settingBlur(event) {
    let t = event.target;
    let p = t.parentNode;
    let n = $("fieldset", t.form).indexOf(p);
    let e = $("input", p).indexOf(t);
    let v = t.value;
    if (!t.attributes.qType) return;
    switch (parseInt(t.attributes.qtype.value)) {
        case (1):
            if (v < 0) v = 0;
        case (2):
            v = Math.round(v);
        case (3):
            break;
        default:
            return;
    }
    ws.send("[4,[" + n + "," + e + "," + t.value + "]]");
}

function gotParameterList(parameters) {
    gotCommandList(parameters);
}

function gotPvList(pvs) {
    let f = $("#pvs")[0];
    empty(f);
    for (let node of pvs) {
        let set = document.createElement("fieldset");
        set.name = node[0];
        set.innerHTML = "<legend>" + node[0] + "</legend>";
        for (let element of node[1]) {
            set.innerHTML += "<input type='button' value='Registrieren' name='" + element + "'> ";
            set.innerHTML += "<label for='" + element + "'>" + element + "</label><br>";
        }
        f.appendChild(set);
    }
    for (let e of $("input", f)) {
        e.addEventListener("click", pvRegister);
    }
}

function pvRegister(event) {
    let t = event.target;
    let p = t.parentNode;
    let n = $("fieldset", t.form).indexOf(p);
    let e = $("input", p).indexOf(t);
    t.value = 0;
    t.type = "number";
    //t.disabled = true;
    ws.send("[6,[" + n + "," + e + "]]");
}

function gotPv(pv) {
    let f = $("#pvs")[0];
    let n = $("fieldset", f)[pv[0]];
    let e = $("input", n)[pv[1]];
    e.value = pv[3];
    e.dispatchEvent(new Event("change"));
}

function clearLog() {
    empty($("#log")[0]);
}
