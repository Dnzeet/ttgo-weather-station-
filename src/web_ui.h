#pragma once

// Halaman pengaturan (WiFi + lokasi). Placeholder %...% diganti di main.cpp
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Weather Station</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;font:16px system-ui,sans-serif;background:#0d2846;color:#e8eef7;padding:20px}
.w{max-width:420px;margin:0 auto}
h1{font-size:22px;margin:0 0 4px}
.m{color:#7f93b0;font-size:13px;margin:0 0 16px}
label{display:block;font-size:13px;color:#9db2cf;margin:14px 0 6px}
input,select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid #27486f;background:#13355a;color:#e8eef7;font:inherit}
.r{display:flex;gap:10px;align-items:flex-end}
.r>div{flex:1}
button{padding:11px 16px;border:0;border-radius:10px;background:#27486f;color:#e8eef7;font:inherit}
.p{width:100%;margin-top:22px;background:#ffb84d;color:#2a1a00;font-weight:600}
</style></head><body><div class="w">
<h1>Weather Station</h1><p class="m">Pengaturan T-Display</p>
<form method="POST" action="/save">
<label>Jaringan WiFi</label>
<select id="ss"><option value="">Memindai...</option></select>
<input id="s" name="ssid" value="%SSID%" placeholder="SSID" style="margin-top:8px">
<label>Password WiFi</label>
<input name="pass" type="password" placeholder="Kosongkan jika tidak diubah">
<label>Kota</label>
<div class="r"><input id="c" name="city" value="%CITY%"><button type="button" id="f">Cari</button></div>
<div class="r"><div><label>Lintang</label><input id="la" name="lat" value="%LAT%"></div>
<div><label>Bujur</label><input id="lo" name="lon" value="%LON%"></div></div>
<p class="m" id="msg" style="margin-top:8px"></p>
<label>Update data tiap</label>
<select name="ivl" id="iv"><option value="5">5 menit</option><option value="10">10 menit</option><option value="15">15 menit</option><option value="30">30 menit</option><option value="60">60 menit</option></select>
<button class="p">Simpan dan restart</button>
</form></div>
<script>
var $=function(i){return document.getElementById(i)};
$('iv').value='%IVL%';
fetch('/scan').then(function(r){return r.json()}).then(function(a){
var s=$('ss');s.innerHTML='<option value="">Pilih jaringan...</option>';
a.forEach(function(n){var o=document.createElement('option');o.value=o.textContent=n;s.appendChild(o)});
}).catch(function(){$('ss').innerHTML='<option value="">Gagal memindai</option>'});
$('ss').onchange=function(){if(this.value)$('s').value=this.value};
$('c').oninput=function(){$('la').value='';$('lo').value=''};
$('f').onclick=function(){
$('msg').textContent='Mencari...';
fetch('https://geocoding-api.open-meteo.com/v1/search?count=1&language=id&name='+encodeURIComponent($('c').value))
.then(function(r){return r.json()}).then(function(d){
if(!d.results){$('msg').textContent='Kota tidak ditemukan';return}
var x=d.results[0];$('c').value=x.name;$('la').value=x.latitude;$('lo').value=x.longitude;
$('msg').textContent=x.name+', '+(x.country||'');
}).catch(function(){$('msg').textContent='HP tidak punya internet. Kosongkan koordinat, perangkat akan mencari sendiri.'});
};
</script></body></html>)HTML";

const char SAVED_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Tersimpan</title>
<style>body{font:18px system-ui,sans-serif;background:#0d2846;color:#e8eef7;padding:40px 20px;text-align:center}</style></head>
<body><h2>Tersimpan</h2><p>Perangkat restart dan menyambung ke WiFi.<br>Sambungkan HP kembali ke WiFi rumah.</p></body></html>)HTML";
