/*
 * ethernet-icon.js
 *
 * Ethernet port SVG icon helpers for FreshTomato web UI.
 *
 * Provides:
 *   updateEthSvg(host, speed, duplex, caption)  — set data-speed / data-duplex
 *   ensureEthSvg(host)                          — create SVG inside host if absent
 *   renderEthIcon(host, speed, duplex, caption) — full render with shadow-DOM setup
 *
 * The SVG template IIFE below is intentionally kept in sync with ethernet.svg.
 * ethernet.svg is the standalone/testable version; this file is the page-integrated one.
 *
 * Fixes/updates (C) 2025 pedro
 * https://freshtomato.org/
 */

;(function () {
	var templateId = 'ethernet-svg-template';
	var templateContent = `<svg width="46px" height="35px" viewBox="0 0 46 35" preserveAspectRatio="xMidYMid meet" shape-rendering="geometricPrecision" text-rendering="geometricPrecision" version="1.1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" data-duplex="HD" data-speed="0">
		<defs>
			<!-- Make outer housing uniform white to remove gradient between outer and inner frames -->
			<linearGradient id="housing-grad" x1="30%" y1="0%" x2="50%" y2="100%">
				<stop stop-color="#ffffff" offset="0%"/>
				<stop stop-color="#ffffff" offset="100%"/>
			</linearGradient>
			<linearGradient id="inner-dark" x1="0%" y1="0%" x2="0%" y2="100%">
				<!-- Use explicit inner-top/inner-bottom vars so we can swap brightness per duplex.
					 FD: top = duplex-end (lighter), bottom = duplex-start (darker).
					 HD/off: top = duplex-start (darker), bottom = duplex-end (lighter). -->
				<stop stop-color="var(--inner-top, var(--duplex-end, #6a6d70ff))" offset="0%"/>
				<stop stop-color="var(--inner-bottom, var(--duplex-start, rgb(47, 49, 50)))" offset="100%"/>
			</linearGradient>
			<!-- angled gradient so secondary color appears toward the bottom-right of the lit LED -->
			<linearGradient id="led-base" x1="25%" y1="5%" x2="100%" y2="100%">
				<stop stop-color="var(--led-primary)" offset="0%"/>
				<stop stop-color="var(--led-secondary)" offset="100%"/>
			</linearGradient>
			<linearGradient id="led-on" xlink:href="#led-base"/>
			<linearGradient id="led-dim" x1="0%" y1="0%" x2="0%" y2="100%">
				<!-- The dim state used for activity blink should match the unused-port LED colors
					 so when blinking, 'off' appears as the same color as a disconnected port. -->
				<stop stop-color="var(--led-off-primary, var(--led-primary))" offset="0%"/>
				<stop stop-color="var(--led-off-secondary, var(--led-secondary))" offset="100%"/>
			</linearGradient>
			<linearGradient id="led-shade" x1="0%" y1="0%" x2="0%" y2="100%">
				<stop offset="0%" stop-color="transparent"/>
				<stop offset="65%" stop-color="transparent"/>
				<stop offset="100%" stop-color="var(--led-shade)" stop-opacity="0.25"/>
			</linearGradient>
			<!-- bottom-right tint using the secondary color to give lit LEDs depth -->
			<radialGradient id="led-bottom" cx="85%" cy="85%" r="60%" fx="85%" fy="85%">
				<stop offset="0%" stop-color="var(--led-secondary)" stop-opacity="0.28"/>
				<stop offset="60%" stop-color="var(--led-secondary)" stop-opacity="0.06"/>
				<stop offset="100%" stop-color="transparent" stop-opacity="0"/>
			</radialGradient>

			<style type="text/css"><![CDATA[
				/* Global off/unused LED colors (used for blink 'off' state) - ~30% brighter overall */
				svg{--led-off-primary: rgb(217, 223, 227);--led-off-secondary: #CED6DAff;--led-off-dim-start: #333333ff;--led-off-dim-end: #2b2b2bff;--led-off-shade: #333333ff}
				/* Darken FD duplex colors by ~50% for stronger contrast in the interior gradient */
				svg[data-duplex="FD"]{--duplex-start: rgb(123, 127, 131);--duplex-end: rgb(39, 41, 44);--pins-fill: #656565ff;--caption-fill: #ffffff;--inner-top:var(--duplex-end);--inner-bottom:var(--duplex-start)}
				/* When HD or off, make the bottom of the inner gradient very bright (almost white).
				For HD/off we swap inner stops so the bottom is the brighter color. */
				svg[data-duplex="HD"]{--duplex-start: #fbfbfcff;--duplex-end: rgb(172, 178, 182);--pins-fill: #919191ff;--caption-fill: #000000;--inner-top:var(--duplex-start);--inner-bottom:var(--duplex-end)}
				/* Neutral / off (unused port) — ~30% brighter overall */
				svg[data-speed="0"]{--led-primary: #B1B2B3ff;--led-secondary: #9A9B9Cff;--led-shade: #333333ff;--led-duration:0s}
				/* Muted amber (brighter + more saturated) */
				svg[data-speed="10"]{--led-primary: #ffb32bff;--led-secondary: rgb(196, 128, 11);--led-shade: #8a4f24ff;--led-duration:3s}
				/* Muted green (slightly darker + more saturated) */
				svg[data-speed="100"]{--led-primary: rgb(32, 177, 25);--led-secondary: rgb(8, 129, 3);--led-shade: #243f18ff;--led-duration:2.5s}
				/* Muted blue (stronger + more saturated) */
				svg[data-speed="1000"]{--led-primary: rgb(40, 117, 233);--led-secondary: rgb(3, 72, 182);--led-shade: #193b58ff;--led-duration:2s}
				/* Muted red for 2.5G (darker) */
				svg[data-speed="2500"]{--led-primary: rgb(232, 63, 51);--led-secondary: rgb(184, 22, 3);--led-shade: #5a1b2eff;--led-duration:1.2s}
				/* Saturated magenta-purple for 5000 (darker + more saturated) */
				svg[data-speed="5000"]{--led-primary: rgb(177, 18, 194);--led-secondary: rgb(105, 1, 170);--led-shade: #1f0420ff;--led-duration:0.9s}
				svg[data-speed="10000"]{--led-primary: #ffffffff;--led-secondary: rgb(192, 192, 192);--led-dim-start: #595959ff;--led-dim-end: #494949ff;--led-shade: #595959ff;--led-duration:0.6s}

				@-webkit-keyframes blink-activity{
					0%,100%{opacity:0}6%{opacity:1}9%{opacity:0}
					17%{opacity:1}28%{opacity:1}35%{opacity:0}
					52%{opacity:1}66%{opacity:0}72%{opacity:1}
					83%{opacity:0}91%{opacity:1}
				}
				@keyframes blink-activity{
					0%,100%{opacity:0}6%{opacity:1}9%{opacity:0}
					17%{opacity:1}28%{opacity:1}35%{opacity:0}
					52%{opacity:1}66%{opacity:0}72%{opacity:1}
					83%{opacity:0}91%{opacity:1}
				}

				.act-led-blink .led-dim{
					opacity:0;
					-webkit-animation-name:blink-activity;
					animation-name:blink-activity;
					-webkit-animation-duration:var(--led-duration, 3s);
					animation-duration:var(--led-duration, 3s);
					-webkit-animation-iteration-count:infinite;
					animation-iteration-count:infinite;
					-webkit-animation-timing-function:step-end;
					animation-timing-function:step-end;
				}
				svg[data-speed="0"] .act-led-blink .led-dim{
					-webkit-animation:none;
					animation:none;
				}
			]]></style>
		</defs>

		<clipPath id="outer-clip">
			<rect width="46" height="35" rx="4" ry="4"/>
		</clipPath>

		<!-- removed semi-transparent outer overlay so SVG corners are fully transparent -->
		<g clip-path="url(#outer-clip)" transform="translate(0,35) scale(1,-1)">
			<rect x="0.75" y="0.75" width="44.5" height="33.5" rx="3.25" ry="3.25" fill="url(#housing-grad)" stroke="#3e4548ff" stroke-width="1"/>
			<path d="M6 6 Q6 5 7 5 L39 5 Q40 5 40 6 L40 22 Q40 23 39 23 L32 23 L32 24 Q32 25 31 25 L29 25 L29 27 Q29 28 28 28 L18 28 Q17 28 17 27 L17 25 L15 25 Q14 25 14 24 L14 23 L7 23 Q6 23 6 22 Z"
				fill="url(#inner-dark)" stroke="#323639ff" stroke-width="0.45"/>
			<g fill="#9ea0a2ff">
				<rect x="12" y="7.45" width="2.8" height="5.95"/>
				<rect x="18.6" y="7.45" width="2.8" height="5.95"/>
				<rect x="25.2" y="7.45" width="2.8" height="5.95"/>
				<rect x="31.8" y="7.45" width="2.8" height="5.95"/>
			</g>
			<!-- LED fills and dim overlays (no strokes) - strokes are drawn last to remain on top -->
			<rect x="3" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="url(#led-on)"/>
			<rect x="3" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="url(#led-bottom)" pointer-events="none" opacity="0.22"/>
			<rect x="3" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="url(#led-shade)" pointer-events="none" opacity="0.15"/>
			<g class="act-led-blink">
				<rect class="led-on" x="33.5" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="url(#led-on)"/>
				<rect class="led-bottom" x="33.5" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="url(#led-bottom)" pointer-events="none" opacity="0.22"/>
				<rect class="led-dim" x="33.5" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="url(#led-dim)" opacity="0.45"/>
				<rect x="33.5" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="url(#led-shade)" pointer-events="none" opacity="0.25"/>
			</g>
			<!-- draw strokes last so edge is always visible above fills/dims -->
			<rect x="3" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="none" stroke="#1e2021ff" stroke-width="0.45"/>
			<rect x="33.5" y="25.5" width="9" height="6" rx="1.5" ry="1.5" fill="none" stroke="#1e2021ff" stroke-width="0.45"/>
		</g>

		<text id="port-caption" x="23" y="20" text-anchor="middle" dominant-baseline="central"
			style="font:bold 12px Calibri,Candara,Arial Narrow,Segoe UI,sans-serif;letter-spacing:-1px;fill:var(--caption-fill,#000);pointer-events:none"></text>
	</svg>`;

	function ensureTemplate() {
		var tpl = document.getElementById(templateId);
		if (tpl)
			return tpl;
		tpl = document.createElement('template');
		tpl.id = templateId;
		tpl.innerHTML = templateContent;
		(document.head || document.documentElement).appendChild(tpl);
		return tpl;
	}

	function ensureEthSvg(host) {
		if (!host)
			return null;
		var svg = host.querySelector('svg');
		if (svg)
			return svg;
		var tpl = ensureTemplate();
		if (!tpl)
			return null;
		if (tpl.content && tpl.content.firstElementChild) {
			svg = tpl.content.firstElementChild.cloneNode(true);
			host.appendChild(svg);
			return svg;
		}
		host.innerHTML = tpl.innerHTML;
		return host.querySelector('svg');
	}

	function updateEthSvg(host, speed, duplex, caption) {
		var svg = ensureEthSvg(host);
		if (!svg)
			return;
		var w = host.getAttribute('data-w');
		var h = host.getAttribute('data-h');
		if (w && h) {
			svg.setAttribute('width', w);
			svg.setAttribute('height', h);
		}
		if (speed !== undefined)
			svg.setAttribute('data-speed', speed);
		if (duplex !== undefined)
			svg.setAttribute('data-duplex', duplex);
		if (caption !== undefined) {
			if (caption !== '')
				svg.setAttribute('data-caption', caption);
			else
				svg.removeAttribute('data-caption');
			var txt = svg.querySelector('#port-caption');
			if (txt)
				txt.textContent = caption;
		}
	}

	/*
	 * renderEthIcon(host, speed, duplex, caption)
	 *
	 * Renders the ethernet port SVG into `host`.  Shadow DOM is created once per
	 * host element (when attachShadow is available) so SVG styles stay isolated
	 * from page CSS.  The actual SVG container is stored in host._ethIconHost.
	 *
	 * Returns `caption` so callers can use it as an element title or tooltip.
	 */
	function renderEthIcon(host, speed, duplex, caption) {
		if (!host)
			return caption;

		/* create shadow DOM once per host element when the browser supports it */
		if (!host._ethIconHost) {
			if (host.attachShadow) {
				var shadow = host.attachShadow({mode: 'open'});
				var inner = document.createElement('div');
				shadow.appendChild(inner);
				host._ethIconHost = inner;
			} else {
				host._ethIconHost = host;
			}
		}

		updateEthSvg(host._ethIconHost, speed, duplex, caption);
		return caption;
	}

	window.ensureEthSvg   = ensureEthSvg;
	window.updateEthSvg   = updateEthSvg;
	window.renderEthIcon  = renderEthIcon;
})();
