// Time zone helpers shared by the main page and the ESP32 /config page.
// The board runs on a POSIX rule ("CST-8"), people pick a place ("Asia/Shanghai").
// timezones.js pairs the two from the tz database and is loaded only when needed.
let zonesLoading = null;

function loadZones() {
  zonesLoading = zonesLoading || new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = "/static/timezones.js";
    script.onload = resolve;
    script.onerror = () => {
      zonesLoading = null;
      reject(new Error("timezones.js"));
    };
    document.head.appendChild(script);
  });
  return zonesLoading;
}

function zoneName(zone, style, locale) {
  try {
    return new Intl.DateTimeFormat(locale, { timeZone: zone, timeZoneName: style })
      .formatToParts(Date.now()).find((part) => part.type === "timeZoneName").value;
  } catch (error) {
    return null;
  }
}

// Minutes east of UTC right now, so summer time shows as it is today.
function zoneOffset(zone) {
  const text = zoneName(zone, "longOffset", "en-US") || "GMT";
  const match = /([+-])(\d\d):(\d\d)/.exec(text);
  return match ? (match[1] === "-" ? -1 : 1) * (Number(match[2]) * 60 + Number(match[3])) : 0;
}

function browserZone() {
  const zone = Intl.DateTimeFormat().resolvedOptions().timeZone;
  if (window.TIMEZONES[zone]) return zone;
  if (window.TIMEZONE_ALIASES[zone]) return window.TIMEZONE_ALIASES[zone];
  const here = -new Date().getTimezoneOffset();
  return Object.keys(window.TIMEZONES).find((name) => zoneOffset(name) === here) || "UTC";
}

// "(UTC+08:00) China Time - Shanghai", ordered by offset the way most systems list them.
let zoneList = null;

function zoneOptions(select, chosen, lang) {
  if (!zoneList || zoneList.lang !== lang) {
    const items = Object.keys(window.TIMEZONES).map((zone) => {
      const offset = zoneOffset(zone);
      const size = Math.abs(offset);
      const utc = `UTC${offset < 0 ? "-" : "+"}${String(Math.floor(size / 60)).padStart(2, "0")}`
        + `:${String(size % 60).padStart(2, "0")}`;
      const generic = zoneName(zone, "longGeneric", lang) || zoneName(zone, "long", lang);
      const place = zone.split("/").slice(1).join(" / ").replace(/_/g, " ");
      // Where the browser has no Chinese name it falls back to an old city
      // name, e.g. "Godthab时间" for Nuuk.
      const named = generic && !/^(GMT|UTC)/.test(generic)
        && !(lang !== "en" && /[A-Za-z]/.test(generic));
      const parts = [named ? generic : null, place || zone];
      return { zone, offset, label: `(${utc}) ${parts.filter(Boolean).join(" - ")}` };
    });
    items.sort((a, b) => a.offset - b.offset || a.label.localeCompare(b.label, lang));
    const quote = (text) => text.replace(/[&<>"']/g, (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
    zoneList = { lang, html: items.map((item) =>
      `<option value="${quote(item.zone)}">${quote(item.label)}</option>`).join("") };
  }
  select.innerHTML = zoneList.html;
  select.value = window.TIMEZONES[chosen] ? chosen : browserZone();
}
