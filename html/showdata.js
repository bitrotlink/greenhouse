var Ifans = 400; // cA. Assumption is that the fans draw more than 4A.
var Ifurnace = 230; // cA
var Ifurnace_least = 150; // cA
var EMA_alpha = 0.8;

// Latest data
var dat = {sec:null,elevation:null,flux_area:null,exch_flow_rate:null,tmp_ctrl_module:null,tmp_outdoor_module:null,fencepost:null,underground:null,ceiling:null,floor_hub:null,heat_exch_inlet:null,heat_exch_outlet:null,delta_exch:null,csp_outlet:null,delta_csp:null,swamp:null,tank_1:null,tank_2:null,tank_3:null,tank_4:null,aux:null,furnace_ctrl:null,furnace_power:null,vent_sys:null,vent:null,swamp_pump:null,vfans:null,tflux:null,tflow:null,plocal:null,psea:null,gas:null,Vrms:null,Irms:null,Pmean:null,S:null,PF:null,hum_in:null,hum_out:null,lux:null,vis:null,ired:null,uva:null,uvb:null,vis_ir:null,wlan0_level:null};

var dependees = { // Values on which at least one other derived value depends
    // Only the keys are used. Null values here just because JS has no sets.
    Vrms:null,
    Irms:null,
    Pmean:null,
    vis_ir:null,
    ired:null,
    uva:null,
    uvb:null
};


var temp_sensors = { // Only the keys used here too
    tmp_ctrl_module:null,
    tmp_outdoor_module:null,
    fencepost:null,
    underground:null,
    ceiling:null,
    floor_hub:null,
    heat_exch_inlet:null,
    heat_exch_outlet:null,
    tank_1:null,
    tank_2:null,
    tank_3:null,
    tank_4:null,
    csp_outlet:null,
    swamp:null,
    aux:null
};

function seaPres() { // Convert local pressure to sea level pressure
    var h = dat['elevation'];
    var P=dat['plocal']/10.0;
    var T=dat['fencepost']/100.0;
    var ch=0.0065*h;
    return P*(Math.pow(1-(ch/(T+ch+273.15)), -5.257));
}

function toMST(sec) { // Convert from UTC to MST
    return sec-(7*3600);
}

function dateString(sec) { // Input Unix time in seconds
    var t = new Date(sec*1000);
    return t.getUTCFullYear().toString() + "-" +
	(t.getUTCMonth()+1).toString() + "-" +
	t.getUTCDate().toString() + "&nbsp&nbsp&nbsp" +
	("0"+t.getUTCHours().toString()).slice(-2) + ":" +
	("0"+t.getUTCMinutes().toString()).slice(-2) + ":" +
	("0"+t.getUTCSeconds().toString()).slice(-2);
}

// All except temp sensors
var sensors = {
    sec: function(c) {
	c.innerHTML=dateString(toMST(dat['sec'])) + " MST"},
    furnace_power: function(c) {c.innerHTML=
			  (dat['furnace_power']==1)?"live":"dead"},
    furnace_ctrl: function(c) {c.innerHTML=
			  (dat['furnace_ctrl']==1)?"ON":"off"},
    vent_sys: function(c) {c.innerHTML=
			(dat['vent_sys']==1)?"OPEN":"closed"},
    vent: function(c) {c.innerHTML=
			(dat['vent']==1)?"OPEN":"closed"},
    swamp_pump: function(c) {c.innerHTML=
			(dat['swamp_pump']==1)?"ON":"off"},
    tflux: function(c) {var tflux = (dat['tflux']);
			var dir = tflux>=0?" ↑":" ↓";
			c.innerHTML=Math.abs(tflux).toString()+dir;
			getCell('tflow', 1).innerHTML="<b>"+
			(Math.abs(tflux)
			 *(dat['flux_area'])/1000).toFixed(0)+dir
			+"</b>"},
    plocal: function(c) {c.innerHTML=(dat['plocal']/10.0).toFixed(1);
			 getCell('psea', 1).innerHTML=(seaPres()).toFixed(1)},
    gas: function(c) {c.innerHTML=dat['gas']},
    Vrms: function(c) {c.innerHTML=(dat['Vrms']).toFixed(0)},
    Irms: function(c) {c.innerHTML=(dat['Irms']/100.0).toFixed(2);
		       var PF = (dat['Pmean']/(dat['Vrms']*dat['Irms']/100.0));
		       var furnace_ctrl = dat['furnace_ctrl'];
		       getCell('vfans', 1).innerHTML=(
			   (PF<0.8) && (dat['tmp_ctrl_module']>1500) && (
			       ((dat['Irms'])>(Ifans+Ifurnace)) ||
				   ((furnace_ctrl==0)
				    && ((dat['Irms'])>Ifans))))
		       ?"ON":"off";
		       var furnace_actual = ((PF<0.92) &&
					     ((dat['Irms'])>(Ifurnace_least)) &&
					     (furnace_ctrl==1))?"ON":"off";
		       getCell('furnace_actual', 1).innerHTML= furnace_actual;
		       document.getElementById('warning').innerHTML =
		       (furnace_ctrl==1 && furnace_actual=="off")?
		       "<span style=\"color:#ff0000;\">WARNING: check furnace</span>":"";
		      },
    Pmean: function(c) {c.innerHTML="<b>"+(dat['Pmean']).toFixed(0)+"</b>";
			getCell('S', 1).innerHTML=(dat['Vrms']*dat['Irms']/100.0).toFixed(0);
			getCell('PF', 1).innerHTML=
			(dat['Pmean']/(dat['Vrms']*dat['Irms']/100.0)).toFixed(2)},
    hum_in: function(c) {c.innerHTML=(dat['hum_in']/10.0).toFixed(0)+"%"},
    hum_out: function(c) {c.innerHTML=(dat['hum_out']/10.0).toFixed(0)+"%"},
    ired: function(c) {c.innerHTML=(dat['ired']/1000000.0).toFixed(3)},
    uva: function(c) {c.innerHTML=(dat['uva']/1000.0).toFixed(3)},
    uvb: function(c) {c.innerHTML=(dat['uvb']/1000.0).toFixed(3)},
    vis_ir: function(c) {getCell('l_tot', 1).innerHTML="<b>"+
			 ((dat['vis_ir']/1000000.0)
			  +(dat['uva']/1000.0)
			  +(dat['uvb']/1000.0)).toFixed(3)+"</b>";
			 var vis = Math.max((dat['vis_ir']-dat['ired']), 0);
			 getCell('vis', 1).innerHTML=
			 (vis/1000000.0).toFixed(3);
			 getCell('lux', 1).innerHTML=
			 (vis*683.0/1000000).toFixed(3);},
    wlan0_level: function(c) {var val = dat['wlan0_level'];
			      c.innerHTML=(val==null)?"none":(val+" dBm")}
};

function getCell(field, i) {
    try {
	return document.getElementById(field).cells[i];
    } catch(err) {
	return null;
    }
}

function updateTables(field, datum) {
    var int_datum = datum=="null"?null:parseInt(datum, 10);
    if(((field in dat) && (dat[field]!=int_datum)) || (field in dependees)) {
	if(field=='Irms') {
	    // Don't need to do Vrms or Pmean too; they're already pretty stable
	    if(dat[field] == null) dat[field]=int_datum; // Initial value
	    else dat[field]=dat[field]*EMA_alpha + int_datum*(1-EMA_alpha);
	}
	else dat[field]=int_datum;
	if(field in temp_sensors) {
	    getCell(field, 1).innerHTML = (datum=="null")?
		"*"+getCell(field, 1).innerHTML
		:((int_datum/100.0).toFixed(2));
	    getCell(field,2).innerHTML = (datum=="null")?
		"*"+getCell(field, 2).innerHTML
		:((int_datum/100.0*9)/5+32).toFixed(2);
	    if(field=="fencepost") getCell('psea', 1).innerHTML=(seaPres()).toFixed(1);
	    if((field=="heat_exch_inlet") || (field=="heat_exch_outlet")) {
		var t_c = null;
		var t_f = null;
		if((dat['heat_exch_outlet']!=null) && (dat['heat_exch_inlet']!=null)) {
		    var t_c = (dat['heat_exch_outlet'] - dat['heat_exch_inlet'])/100.0;
		    var t_f = t_c*9/5;
		}
		var dir = t_c<0?" ↑":" ↓";
		getCell('delta_exch', 1).innerHTML = (t_c==null)?
		    "n/a":(t_c.toFixed(2));
		getCell('delta_exch', 2).innerHTML = (t_f==null)?
		    "n/a":(t_f.toFixed(2));
		getCell('heat_exch', 1).innerHTML = (t_c==null)?
		    "n/a":("<b>"+((dat['exch_flow_rate'])*4.184/1000.0
				  *Math.abs(t_c)).toFixed(0)+dir+"</b>");
	    }
	} else if(field=='sec')
	    sensors[field](document.getElementById(field));
	else if(field in sensors) sensors[field](getCell(field, 1));
    }
}

function processRecord(sheading, sdata) {
    var fields = sheading.split(',');
    var data = sdata.split(',');
    var len = fields.length;
    // console.log(`Lengths: ${len}, ${data.length}`);
    for(var i=0; i<len; i++) {
	var field = fields[i];
	var datum = data[i];
	updateTables(field, datum);
    }
    heartbeat=1;
    document.getElementById('status').innerHTML = "connected";
}

var e_lock = 0;
var reqSerial = 0;
var ws = null
var lastMod = null;

var initXHR = new XMLHttpRequest();
initXHR.onreadystatechange = function() {
    if(this.readyState === this.HEADERS_RECEIVED)
	lastMod = initXHR.getResponseHeader("Last-Modified");
};

var watchdogXHR = new XMLHttpRequest();
watchdogXHR.onreadystatechange = function() {
    if(this.readyState === this.HEADERS_RECEIVED) {
	var newLastMod = watchdogXHR.getResponseHeader("Last-Modified");
	if((lastMod != newLastMod)
	   // Don't try to refresh if there isn't even a connection
	   && document.getElementById('status').innerHTML == "connected") {
	    lastMod = newLastMod;
	    if(ws) ws.close();
	    ws = null;
	    location.reload(true);
	}
    }
};

var mqtt_client = new Paho.MQTT.Client(location.hostname, 9001, "showdata_" + (Math.floor(Math.random() * 1000000)+1));

function mqtt_onconnect() {
  // console.log("mqtt_onconnect");
  mqtt_client.subscribe("hb/westmod");
  mqtt_client.subscribe("act/#");
  mqtt_client.subscribe("sensors/#");
}

function mqtt_onfailure() {
    // TODO
}

function mqtt_onconnectionLost(responseObject) {
  if (responseObject.errorCode !== 0) {
    // console.log("mqtt_onconnectionLost:"+responseObject.errorMessage);
      getCell('vent', 1).innerHTML = "n/a";
      getCell('swamp_pump', 1).innerHTML = "n/a";
      // FIXME: do updateTables instead of manually setting n/a, and have the former set n/a for all receipt of null.
      updateTables("swamp", "null");
  }
}

function onMessageArrived(message) {
    // console.log("onMessageArrived:"+message.destinationName+" "+message.payloadString);
    var segments = message.destinationName.split('/');
    if(message.destinationName == "hb/westmod") {
	// TODO: display westmod uptime
    } else if(message.destinationName == "act/westmod") {
	if(message.payloadString == "v") updateTables("vent_sys", "0");
	else if(message.payloadString == "V") updateTables("vent_sys", "1");
    } else if(segments[0] == "sensors") {
	if((segments[1] == "vent") || (segments[1] == "swamp_pump")) {
	    updateTables(segments[1], message.payloadString);
	} else if(message.destinationName == "sensors/28-01142ff394f3/data") {
	    // swamp sensor. Temporary handling.
	    if(message.payloadString == "offline") updateTables("swamp", "null");
	    else updateTables("swamp", message.payloadString);
	}
    }
}

function init() {
    reqSerial++; // Prevent caching
    initXHR.open("HEAD", (document.location)+"?initserial="+reqSerial, true);
    initXHR.send();
    var sec = dat['sec'];
    document.getElementById('status').innerHTML =
	(sec?("<b>stalled</b> (last data at "
	      + dateString(toMST(sec)) + " MST)")
	 :"<b>not connected</b>");
    document.getElementById('sec').innerHTML = "";
    if(ws) ws.close();
    ws = new WebSocket("ws://" + (location.hostname) + "/data_stream");
    ws.onopen = function() {
    };
    ws.onclose = function() {
	document.getElementById('status').innerHTML =
	    dat['sec']==null?"<b>disconnected</b> (no data received)":
	    "<b>disconnected</b> (last data at "
	    + dateString(toMST(dat['sec'])) + " MST)";
	document.getElementById('sec').innerHTML = "";
    };
    ws.onmessage=function(e) {
	if(e_lock==1) document.getElementById('status').innerHTML = "LOCKED";
	e_lock = 1;
	var rows = e.data.split('\n'); // Should be just 1, but just in case...
	for(var i=0; i<rows.length; i++) {
	    // Format is comma-separated headers, then '!', then comma-separated values
	    var pair = rows[i].split('!');
	    processRecord(pair[0], pair[1]);
	}
	e_lock = 0;
    }
    try {
	mqtt_client.disconnect();
    } catch(err) {}
    mqtt_client.onConnectionLost = mqtt_onconnectionLost;
    mqtt_client.onMessageArrived = onMessageArrived;
    mqtt_client.connect({onSuccess:mqtt_onconnect, onFailure:mqtt_onfailure});
}

var heartbeat = 0;
function watchdog() {
    if(!heartbeat) {
	init();
    }
    heartbeat=0;
    reqSerial++;
    watchdogXHR.open("HEAD", (document.location)+"?watchserial="+reqSerial, true);
    watchdogXHR.send();
}
setInterval("watchdog()", 15000);
init();

// message = new Paho.MQTT.Message("Hello");
// message.destinationName = "World";
// mqtt_client.send(message);
