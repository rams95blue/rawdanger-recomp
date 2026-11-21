// ==UserScript==
// @name         RetroAchievements - Badge + Logic Exporter (rawdanger-recomp)
// @namespace    https://retroachievements.org/
// @version      3.2.0
// @description  Export achievement icons (color + grayscale), names, descriptions, authors AND each achievement's unlock logic (what the game must do), readable, plus an offline set file for rawdanger-recomp.
// @match        https://retroachievements.org/*
// @match        https://www.retroachievements.org/*
// @run-at       document-idle
// @grant        GM_xmlhttpRequest
// @grant        GM_info
// @connect      retroachievements.org
// @connect      www.retroachievements.org
// @connect      media.retroachievements.org
// @connect      i.retroachievements.org
// @connect      static.retroachievements.org
// @connect      s3.retroachievements.org
// ==/UserScript==

(() => {
  'use strict';

  // 1. Preserve the original functionality: show the full-color badge on the website.
  const LOCK_SUFFIX = /_lock(?=\.(?:png|jpe?g|gif|webp)(?:[?#\s]|$))/gi;
  const IMAGE_ATTRIBUTES = ['src', 'srcset', 'data-src', 'data-srcset', 'data-lazy-src'];
  const PANEL_ID = 'ra-color-export-panel-v2';
  const STYLE_ID = 'ra-color-export-style-v2';
  const SCRIPT_FILE = 'RetroAchievements_Badge_Logic_Exporter_v3.user.js';
  // Self-contained bundle: the export ZIP includes the exact userscript that made it.
  // The build step inserts an escaped copy of this source into SELF_COPY_BODY.
  const SELF_COPY_BODY = null;  // v3: the copy comes from Tampermonkey (GM_info.scriptSource)
  function bundledScriptSource() {
    try { if (typeof GM_info !== 'undefined' && GM_info.scriptSource) return GM_info.scriptSource; } catch (_) { }
    return '';  // some script managers keep the source to themselves: then the ZIP simply has no copy
  }
  const encoder = new TextEncoder();
  let exporting = false;

  function showColoredBadge(node) {
    if (!(node instanceof Element) || !node.matches('img, source')) return;
    for (const attr of IMAGE_ATTRIBUTES) {
      const oldValue = node.getAttribute(attr);
      if (!oldValue || !/_lock/i.test(oldValue) || !/\/Badge\//i.test(oldValue)) continue;
      const newValue = oldValue.replace(LOCK_SUFFIX, '');
      if (newValue !== oldValue) node.setAttribute(attr, newValue);
    }
    if (node.matches('img') && /\/Badge\//i.test(node.getAttribute('src') || '')) {
      node.style.setProperty('filter', 'none', 'important');
      node.style.setProperty('opacity', '1', 'important');
    }
  }

  function scanImages(root) {
    if (!(root instanceof Document || root instanceof Element)) return;
    if (root instanceof Element) showColoredBadge(root);
    root.querySelectorAll('img, source').forEach(showColoredBadge);
  }

  scanImages(document);
  new MutationObserver((records) => {
    for (const record of records) {
      if (record.type === 'attributes') {
        showColoredBadge(record.target);
      } else {
        for (const node of record.addedNodes) {
          if (node.nodeType === Node.ELEMENT_NODE) scanImages(node);
        }
      }
    }
  }).observe(document.documentElement, {
    subtree: true,
    childList: true,
    attributes: true,
    attributeFilter: IMAGE_ATTRIBUTES,
  });

  // 2. Read the public game page's own embedded data, including the unpublished view.
  //    No API key, login credentials, third-party ZIP library, or external server required.
  function currentGameId() {
    const match = /^\/game\/(\d+)\/?$/.exec(location.pathname);
    return match ? match[1] : null;
  }

  function gamePageUrl(unpublished) {
    const url = new URL(location.href);
    url.searchParams.delete('unpublished');
    // The selected achievement set (?set=...) is intentionally preserved.
    if (unpublished) url.searchParams.set('unpublished', 'true');
    url.hash = '';
    return url.href;
  }

  function readPageJson(raw) {
    if (!raw || !raw.trim().startsWith('{')) return null;
    try {
      const parsed = JSON.parse(raw);
      return parsed && parsed.props && typeof parsed.props === 'object' ? parsed : null;
    } catch (_) {
      return null;
    }
  }

  async function fetchGamePage(url) {
    const response = await fetch(url, {
      credentials: 'include',
      cache: 'no-store',
      headers: { Accept: 'text/html, application/xhtml+xml, application/json' },
    });
    if (!response.ok) throw new Error(`Game metadata request failed (HTTP ${response.status})`);
    const body = await response.text();
    let page = readPageJson(body.trim());
    if (!page) {
      const doc = new DOMParser().parseFromString(body, 'text/html');
      // Inertia/Laravel commonly stores its initial page JSON in #app[data-page].
      const embedded = doc.querySelector('#app[data-page], [data-page]');
      page = readPageJson(embedded?.getAttribute('data-page') || '');
      if (!page) {
        const script = doc.querySelector('script[data-page="app"][type="application/json"], script#page-data');
        page = readPageJson(script?.textContent || '');
      }
    }
    if (!page) throw new Error('RetroAchievements game-page metadata not found; website markup may have changed.');
    return page;
  }

  function getAchievements(page, unofficial) {
    const props = page.props || {};
    const game = props.game || {};
    const sets = Array.isArray(game.gameAchievementSets) ? game.gameAchievementSets : [];
    const unique = new Map();
    for (const set of sets) {
      const collection = set?.achievementSet?.achievements || set?.achievements || [];
      const achievements = Array.isArray(collection) ? collection :
        (Array.isArray(collection.data) ? collection.data :
          (Array.isArray(collection.items) ? collection.items : []));
      for (const achievement of achievements) {
        if (!achievement || achievement.id == null) continue;
        if (unofficial && achievement.isPromoted === true) continue;
        if (!unofficial && achievement.isPromoted === false) continue;
        unique.set(String(achievement.id), achievement);
      }
    }
    return [...unique.values()];
  }

  function gameTitle(page) {
    return String(page.props?.game?.title ||
      document.querySelector('[data-testid="game-show"] h1')?.textContent ||
      document.querySelector('h1')?.textContent ||
      `Game ${currentGameId()}`).trim();
  }

  function cleanFileName(value, fallback = 'Achievement') {
    let result = String(value || fallback)
      .normalize('NFC')
      .replace(/[<>:"/\\|?*\x00-\x1F\x7F]/g, '_')
      .replace(/\s+/g, ' ')
      .replace(/[. ]+$/g, '')
      .trim();
    if (!result || /^\.+$/.test(result)) result = fallback;
    // Conservative limit for Windows paths, while retaining the title.
    result = Array.from(result).slice(0, 110).join('').replace(/[. ]+$/g, '');
    if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i.test(result)) result = `_${result}`;
    return result || fallback;
  }

  // Display names can appear as strings or user objects depending on the site's version.
  function getAuthor(achievement) {
    const candidates = [
      achievement.author, achievement.authorName, achievement.authorDisplayName,
      achievement.developer, achievement.developerUser, achievement.createdBy,
      achievement.creator, achievement.user,
    ];
    function nameOf(value) {
      if (typeof value === 'string' && value.trim()) return value.trim();
      if (value && typeof value === 'object') {
        const n = value.displayName || value.display_name || value.username ||
          value.userName || value.name || value.User;
        return typeof n === 'string' && n.trim() ? n.trim() : '';
      }
      return '';
    }
    for (const candidate of candidates) {
      const name = nameOf(candidate);
      if (name) return name;
    }
    return 'Not provided by page';
  }

  function resolveBadgeUrl(raw) {
    if (!raw) return '';
    let url;
    try { url = new URL(String(raw), location.origin); }
    catch (_) { return ''; }
    if (!['https:', 'http:'].includes(url.protocol)) return '';
    // Only trust site-owned media hosts for binary badge downloads.
    if (!/(^|\.)retroachievements\.org$/i.test(url.hostname)) return '';
    return url.href;
  }

  function badgeUrl(achievement) {
    let raw = achievement.badgeUnlockedUrl || achievement.badgeLockedUrl || achievement.badgeUrl || '';
    if (!raw && achievement.badgeName) raw = `https://media.retroachievements.org/Badge/${achievement.badgeName}.png`;
    if (!raw) return '';
    return resolveBadgeUrl(String(raw).replace(LOCK_SUFFIX, ''));
  }

  function lockedBadgeUrl(achievement, unlockedUrl) {
    const supplied = resolveBadgeUrl(achievement.badgeLockedUrl || '');
    if (supplied && /_lock\.(png|jpe?g|gif|webp)(?:[?#]|$)/i.test(supplied)) return supplied;
    if (!unlockedUrl) return '';
    return unlockedUrl.replace(/\.(png|jpe?g|gif|webp)(?=[?#]|$)/i, '_lock.$1');
  }

  function badgeExtension(url) {
    const match = /\.(png|jpe?g|gif|webp)(?:[?#]|$)/i.exec(url);
    return match ? `.${match[1].toLowerCase().replace('jpeg', 'jpg')}` : '.png';
  }

  function prepareEntries(raw, category) {
    const used = new Set();
    return raw.map((item) => {
      const title = String(item.title || `Achievement ${item.id}`).trim();
      const colorUrl = badgeUrl(item);
      const grayUrl = lockedBadgeUrl(item, colorUrl);
      const ext = badgeExtension(colorUrl);
      const base = cleanFileName(title, `Achievement ${item.id}`);
      let filename = `${base}${ext}`;
      if (used.has(filename.toLocaleLowerCase())) filename = `${base} (${item.id})${ext}`;
      let n = 2;
      while (used.has(filename.toLocaleLowerCase())) filename = `${base} (${item.id}-${n++})${ext}`;
      used.add(filename.toLocaleLowerCase());
      return {
        id: String(item.id),
        title,
        author: getAuthor(item),
        description: String(item.description || '').trim(),
        points: item.points ?? 0,
        category,
        filename,
        variants: [
          { label: 'Color', url: colorUrl, path: `${category}/Color/${filename}`, bytes: null, error: '' },
          { label: 'Grayscale', url: grayUrl, path: `${category}/Grayscale/${filename}`, bytes: null, error: '' },
        ],
      };
    });
  }

  // 3. Retrieve the actual color artwork, rather than attempting to remove
  //    grayscale from the locked PNG. GM_xmlhttpRequest handles badge CDN CORS.
  function gmImageRequest(url) {
    return new Promise((resolve, reject) => {
      if (typeof GM_xmlhttpRequest !== 'function') {
        reject(new Error('Tampermonkey image request permission is missing.'));
        return;
      }
      GM_xmlhttpRequest({
        method: 'GET',
        url,
        responseType: 'arraybuffer',
        timeout: 30000,
        onload: (response) => {
          if (response.status < 200 || response.status >= 300) {
            reject(new Error(`HTTP ${response.status}`));
            return;
          }
          const raw = response.response;
          if (!raw || typeof raw.byteLength !== 'number' || raw.byteLength === 0) {
            reject(new Error('Empty or non-binary badge response'));
            return;
          }
          const bytes = new Uint8Array(raw);
          const isPng = bytes.length > 8 && bytes[0] === 137 && bytes[1] === 80 && bytes[2] === 78 && bytes[3] === 71;
          const isJpeg = bytes.length > 2 && bytes[0] === 255 && bytes[1] === 216;
          const isGif = bytes.length > 5 && String.fromCharCode(...bytes.slice(0, 3)) === 'GIF';
          const isWebp = bytes.length > 12 && String.fromCharCode(...bytes.slice(0, 4)) === 'RIFF' && String.fromCharCode(...bytes.slice(8, 12)) === 'WEBP';
          if (!isPng && !isJpeg && !isGif && !isWebp) {
            reject(new Error('Badge URL did not return an image'));
            return;
          }
          resolve(bytes);
        },
        onerror: () => reject(new Error('Network error or missing @connect permission')),
        ontimeout: () => reject(new Error('Request timed out')),
      });
    });
  }

  function delay(ms) { return new Promise((resolve) => setTimeout(resolve, ms)); }

  async function downloadBadges(variants, onProgress) {
    let next = 0;
    let done = 0;
    const workers = Array.from({ length: Math.min(4, variants.length) }, async () => {
      while (next < variants.length) {
        const variant = variants[next++];
        if (!variant.url) variant.error = 'No badge image URL supplied by RetroAchievements';
        else {
          for (let attempt = 1; attempt <= 3; attempt++) {
            try {
              variant.bytes = await gmImageRequest(variant.url);
              variant.error = '';
              break;
            } catch (error) {
              variant.error = error.message || String(error);
              if (attempt < 3) await delay(attempt * 400);
            }
          }
        }
        done++;
        onProgress(done, variants.length);
      }
    });
    await Promise.all(workers);
  }

  // 4. Simple standards-compliant ZIP writer (STORE method). PNGs are already
  //    compressed, so we don't add an external dependency like JSZip.
  const CRC_TABLE = (() => {
    const table = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let c = i;
      for (let j = 0; j < 8; j++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      table[i] = c >>> 0;
    }
    return table;
  })();

  function crc32(data) {
    let crc = 0xFFFFFFFF;
    for (let i = 0; i < data.length; i++) crc = CRC_TABLE[(crc ^ data[i]) & 255] ^ (crc >>> 8);
    return (crc ^ 0xFFFFFFFF) >>> 0;
  }

  function zipArchive(files) {
    if (files.length > 65535) throw new Error('Too many files for ZIP format');
    const date = new Date();
    const dosTime = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1);
    const dosDate = ((Math.max(1980, date.getFullYear()) - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
    const chunks = [];
    const centralChunks = [];
    let offset = 0;
    let centralSize = 0;

    for (const file of files) {
      const nameBytes = encoder.encode(file.name);
      const data = file.data instanceof Uint8Array ? file.data : encoder.encode(file.data);
      const crc = crc32(data);
      if (data.byteLength > 0xFFFFFFFF || nameBytes.byteLength > 65535) throw new Error('ZIP entry is too large');

      const local = new Uint8Array(30);
      const l = new DataView(local.buffer);
      l.setUint32(0, 0x04034B50, true);
      l.setUint16(4, 20, true);
      l.setUint16(6, 0x0800, true); // UTF-8 names
      l.setUint16(8, 0, true);      // STORE (no recompression)
      l.setUint16(10, dosTime, true);
      l.setUint16(12, dosDate, true);
      l.setUint32(14, crc, true);
      l.setUint32(18, data.byteLength, true);
      l.setUint32(22, data.byteLength, true);
      l.setUint16(26, nameBytes.byteLength, true);
      chunks.push(local, nameBytes, data);

      const center = new Uint8Array(46);
      const c = new DataView(center.buffer);
      c.setUint32(0, 0x02014B50, true);
      c.setUint16(4, 20, true);
      c.setUint16(6, 20, true);
      c.setUint16(8, 0x0800, true);
      c.setUint16(10, 0, true);
      c.setUint16(12, dosTime, true);
      c.setUint16(14, dosDate, true);
      c.setUint32(16, crc, true);
      c.setUint32(20, data.byteLength, true);
      c.setUint32(24, data.byteLength, true);
      c.setUint16(28, nameBytes.byteLength, true);
      c.setUint32(42, offset, true);
      centralChunks.push(center, nameBytes);
      centralSize += center.byteLength + nameBytes.byteLength;
      offset += local.byteLength + nameBytes.byteLength + data.byteLength;
      if (offset > 0xFFFFFFFF) throw new Error('ZIP archive exceeds 4 GB');
    }

    const end = new Uint8Array(22);
    const e = new DataView(end.buffer);
    e.setUint32(0, 0x06054B50, true);
    e.setUint16(8, files.length, true);
    e.setUint16(10, files.length, true);
    e.setUint32(12, centralSize, true);
    e.setUint32(16, offset, true);
    return new Blob([...chunks, ...centralChunks, end], { type: 'application/zip' });
  }

  // 5. Text index and standalone, local HTML gallery.

  // 5. rawdanger-recomp: each achievement's LOGIC - the memory conditions RetroAchievements checks to unlock it ("MemAddr", the
  //    trigger). Nothing here needs a password: the script reads what the website gives the browser you are already signed in
  //    with. Where the site has it, in this order:
  //    a) the achievement data on the game page / the achievement's own page (some site versions include the trigger);
  //    b) RetroAchievements' client set (dorequest.php r=patch) with the sign-in token the site keeps for your session, when
  //       the page data carries one;
  //    the code notes (what each memory address is) make the conditions readable. What could not be found is said plainly
  //    in Achievements_Logic.txt; the badges and texts are exported either way.
  const TRIGGER_KEYS = /^(memaddr|mem_addr|trigger|triggerdefinition|trigger_definition|definition|logic|conditions?)$/i;
  const LOOKS_LIKE_LOGIC = /(^|[_|SRPABCNOMZIQKGTD:])\s*(0x[HXLUMNOPQRSTWIJGKhxlumnopqrstwijgk ]?[0-9a-fA-F]+|f[FfBbHhIiMm][0-9a-fA-F]+)\s*(=|!=|<=|>=|<|>)/;

  function findTrigger(value, depth = 0) {
    if (depth > 6 || value == null) return '';
    if (typeof value === 'string') return LOOKS_LIKE_LOGIC.test(value) ? value : '';
    if (Array.isArray(value)) { for (const v of value) { const t = findTrigger(v, depth + 1); if (t) return t; } return ''; }
    if (typeof value === 'object') {
      for (const [k, v] of Object.entries(value)) if (TRIGGER_KEYS.test(k) && typeof v === 'string' && LOOKS_LIKE_LOGIC.test(v)) return v;
      for (const [k, v] of Object.entries(value)) if (TRIGGER_KEYS.test(k) && v && typeof v === 'object') { const t = findTrigger(v, depth + 1); if (t) return t; }
      for (const v of Object.values(value)) if (v && typeof v === 'object') { const t = findTrigger(v, depth + 1); if (t) return t; }
    }
    return '';
  }

  function findToken(page) {
    // the signed-in user's client token, when the site's shared page data carries one (names differ between site versions)
    const user = page?.props?.auth?.user || page?.props?.user || null;
    if (!user || typeof user !== 'object') return null;
    const name = user.username || user.User || user.displayName || user.display_name || '';
    let token = '';
    (function walk(o, depth) {
      if (!o || typeof o !== 'object' || depth > 3 || token) return;
      for (const [k, v] of Object.entries(o)) {
        if (typeof v === 'string' && /^(connect|app|api)?_?token$/i.test(k) && /^[A-Za-z0-9]{12,64}$/.test(v)) { token = v; return; }
        if (v && typeof v === 'object') walk(v, depth + 1);
      }
    })(user, 0);
    return name && token ? { name, token } : null;
  }

  async function fetchClientSet(gameId, who) {
    const body = `r=patch&u=${encodeURIComponent(who.name)}&t=${encodeURIComponent(who.token)}&g=${encodeURIComponent(gameId)}`;
    const response = await fetch('/dorequest.php', { method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    if (!data?.Success || !Array.isArray(data?.PatchData?.Achievements)) throw new Error(data?.Error || 'no set in the answer');
    return data;
  }

  async function fetchAchievementPage(id) {
    try { return await fetchGamePage(new URL(`/achievement/${id}`, location.origin).href); } catch (_) { return null; }
  }

  async function fetchCodeNotes(gameId) {
    // the code notes: address -> what the set's authors found there. Public on the website.
    const notes = new Map();
    for (const path of [`/game/${gameId}/code-notes`, `/codenotes.php?g=${gameId}`]) {
      try {
        const response = await fetch(new URL(path, location.origin).href, { credentials: 'include', headers: { Accept: 'text/html, application/json' } });
        if (!response.ok) continue;
        const body = await response.text();
        let page = readPageJson(body.trim());
        if (!page) {
          const doc = new DOMParser().parseFromString(body, 'text/html');
          page = readPageJson(doc.querySelector('#app[data-page], [data-page]')?.getAttribute('data-page') || '');
          if (!page) {
            // the older page: a table of address / note rows
            for (const row of doc.querySelectorAll('tr')) {
              const cells = row.querySelectorAll('td');
              if (cells.length < 2) continue;
              const m = /0x([0-9a-f]+)/i.exec(cells[0].textContent || '');
              if (m) notes.set(parseInt(m[1], 16), (cells[cells.length - 1].textContent || '').trim());
            }
          }
        }
        if (page) {
          (function walk(o, depth) {
            if (!o || typeof o !== 'object' || depth > 6) return;
            if (Array.isArray(o)) { for (const v of o) walk(v, depth + 1); return; }
            const address = o.address ?? o.Address ?? o.addr;
            const note = o.note ?? o.Note ?? o.body;
            if (address != null && typeof note === 'string') {
              const a = typeof address === 'number' ? address : parseInt(String(address).replace(/^0x/i, ''), 16);
              if (!Number.isNaN(a)) notes.set(a, note.trim());
            }
            for (const v of Object.values(o)) if (v && typeof v === 'object') walk(v, depth + 1);
          })(page.props, 0);
        }
        if (notes.size) break;
      } catch (_) { }
    }
    return notes;
  }

  // ---- reading a trigger as plain words -------------------------------------------------------------------------------
  const SIZES = { '': '16-bit', H: '8-bit', X: '32-bit', W: '24-bit', L: 'low 4 bits of', U: 'high 4 bits of', M: 'bit 0 of', N: 'bit 1 of',
    O: 'bit 2 of', P: 'bit 3 of', Q: 'bit 4 of', R: 'bit 5 of', S: 'bit 6 of', T: 'bit 7 of', K: 'number of set bits in',
    I: '16-bit big-endian', J: '24-bit big-endian', G: '32-bit big-endian' };
  const FLAGS = { R: 'Reset if', P: 'Pause if', A: 'Add source', B: 'Sub source', C: 'Add hits', D: 'Sub hits', N: 'And next',
    O: 'Or next', M: 'Measured', Q: 'Measured if', I: 'Add address', T: 'Trigger', Z: 'Reset next if', K: 'Remember', G: 'Measured %' };
  const OPS = { '=': 'equals', '!=': 'is not', '<': 'is below', '<=': 'is at most', '>': 'is above', '>=': 'is at least' };

  function operand(text, notes) {
    let t = text.trim();
    let kind = 'value of';
    if (/^d/i.test(t)) { kind = 'previous frame\'s'; t = t.slice(1); }
    else if (/^p/i.test(t)) { kind = 'prior (last different) value of'; t = t.slice(1); }
    else if (/^b/i.test(t)) { kind = 'BCD'; t = t.slice(1); }
    else if (/^~/.test(t)) { kind = 'inverted'; t = t.slice(1); }
    const m = /^0x([HXLUMNOPQRSTWIJGK ]?)([0-9a-f]+)$/i.exec(t);
    if (m) {
      const address = parseInt(m[2], 16);
      const size = SIZES[m[1].trim().toUpperCase()] ?? m[1];
      const note = notes.get(address);
      const label = note ? ` (${note.split(/\r?\n/)[0].slice(0, 70)})` : '';
      return `${kind === 'value of' ? '' : kind + ' '}${size} at 0x${address.toString(16).toUpperCase().padStart(8, '0')}${label}`;
    }
    if (/^h[0-9a-f]+$/i.test(t)) return `${parseInt(t.slice(1), 16)}`;
    if (/^\{recall\}$/i.test(t)) return 'the remembered value';
    return t;
  }

  function readable(trigger, notes) {
    if (!trigger) return [];
    const out = [];
    const groups = trigger.split(/(?<!0x)S/);
    groups.forEach((group, gi) => {
      out.push(gi === 0 ? 'Core (all of these):' : `Alternative ${gi} (or all of these):`);
      for (const raw of group.split('_')) {
        const c = /^([A-Z]:)?(.+?)(?:(=|!=|<=|>=|<|>)(.+?))?(?:\.(\d+)\.)?$/.exec(raw.trim());
        if (!c) { out.push(`  - ${raw}`); continue; }
        const flag = c[1] ? FLAGS[c[1][0]] || c[1][0] : '';
        let text = operand(c[2], notes);
        if (c[3]) text += ` ${OPS[c[3]]} ${operand(c[4], notes)}`;
        if (c[5] && c[5] !== '0') text += `, ${c[5]} time${c[5] === '1' ? '' : 's'} (hit count)`;
        out.push(`  - ${flag ? flag + ': ' : ''}${text}`);
      }
    });
    return out;
  }

  async function collectLogic(gameId, officialPage, entries, onProgress) {
    const report = { source: 'none', notes: new Map(), problems: [] };
    // a) the page data the badges came from
    const raw = new Map();
    for (const a of [...getAchievements(officialPage, false), ...getAchievements(officialPage, true)]) {
      const t = findTrigger(a);
      if (t) raw.set(String(a.id), t);
    }
    if (raw.size) report.source = 'the game page';
    // b) the client set: first as the signed-in browser itself (the site may accept its session for this request), then with
    //    the session's token when the page data carries one
    if (raw.size < entries.length) {
      for (const body of [`r=patch&g=${encodeURIComponent(gameId)}`, `r=achievementsets&g=${encodeURIComponent(gameId)}`]) {
        try {
          const response = await fetch('/dorequest.php', { method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
          const data = await response.json();
          const list = data?.PatchData?.Achievements || data?.Sets?.flatMap?.((x) => x.Achievements || []) || [];
          if (data?.Success && list.length) {
            report.clientSet = data.PatchData ? data : { Success: true, PatchData: { ID: Number(gameId), Title: '', Achievements: list } };
            for (const a of list) if (a.MemAddr) raw.set(String(a.ID), a.MemAddr);
            report.source = 'RetroAchievements\' client set (your browser session)';
            break;
          }
          report.problems.push(`client set (${body.split('&')[0]}): ${data?.Error || 'no achievements in the answer'}`);
        } catch (error) { report.problems.push(`client set (${body.split('&')[0]}): ${error.message || error}`); }
      }
    }
    if (raw.size < entries.length) {
      const who = findToken(officialPage);
      if (who) {
        try {
          const data = await fetchClientSet(gameId, who);
          report.clientSet = data;
          for (const a of data.PatchData.Achievements) if (a.MemAddr) raw.set(String(a.ID), a.MemAddr);
          report.source = 'RetroAchievements\' client set (your signed-in session)';
        } catch (error) { report.problems.push(`client set: ${error.message || error}`); }
      } else report.problems.push('client set: the page data carries no sign-in token for this browser (signed out, or the site keeps it elsewhere)');
    }
    // c) each achievement's own page
    if (raw.size < entries.length) {
      let done = 0;
      for (const e of entries) {
        if (raw.has(e.id)) continue;
        const page = await fetchAchievementPage(e.id);
        const t = page ? findTrigger(page.props?.achievement || page.props) : '';
        if (t) { raw.set(e.id, t); if (report.source === 'none') report.source = 'the achievement pages'; }
        onProgress(++done);
        await delay(120);
      }
    }
    report.notes = await fetchCodeNotes(gameId);
    for (const e of entries) e.logic = raw.get(e.id) || '';
    report.found = entries.filter((e) => e.logic).length;
    return report;
  }

  function makeLogicText(title, gameUrl, entries, report) {
    const lines = [
      `GAME: ${title}`, `SOURCE: ${gameUrl}`, `EXPORTED: ${new Date().toISOString()}`,
      `LOGIC FOUND: ${report.found} / ${entries.length}  (from ${report.source})`,
      `CODE NOTES: ${report.notes.size} addresses`,
      ...report.problems.map((p) => `NOTE: ${p}`),
      '',
      'Each achievement: its unlock logic in RetroAchievements\' own syntax (LOGIC), then the same in words. Addresses are',
      'the PS2\'s main memory; the text in brackets is the set authors\' code note for that address.',
      'rawdanger-recomp: put this whole folder in launcher\\art; RawDangerPC.exe finds RA_Set_*.json and the game checks the set offline.',
      '',
    ];
    for (const e of entries) {
      lines.push(`==== ${e.title}  (${e.category}, ${e.points} points, ID ${e.id})`, e.description);
      if (e.logic) lines.push(`LOGIC: ${e.logic}`, ...readable(e.logic, report.notes));
      else lines.push('LOGIC: (not found on the website for this achievement)');
      lines.push('');
    }
    return '﻿' + lines.join('\r\n');
  }

  function makeOfflineSet(gameId, title, entries, report) {
    // RetroAchievements' client format (what an emulator receives), read by RawDangerPC.exe and the game
    if (report.clientSet) return JSON.stringify(report.clientSet);
    const achievements = entries.filter((e) => e.logic).map((e) => ({
      ID: Number(e.id), MemAddr: e.logic, Title: e.title, Description: e.description, Points: Number(e.points) || 0,
      Author: e.author, BadgeName: (/Badge\/(\d+)/.exec(e.variants[0].url) || [])[1] || '', Flags: e.category === 'Official' ? 3 : 5,
    }));
    return JSON.stringify({ Success: true, PatchData: { ID: Number(gameId), Title: title, Achievements: achievements } });
  }

  function makeTextIndex(title, gameUrl, entries) {
    const official = entries.filter((e) => e.category === 'Official');
    const unofficial = entries.filter((e) => e.category === 'Unofficial');
    const variants = entries.flatMap((e) => e.variants);
    const lines = [
      `GAME: ${title}`,
      `SOURCE: ${gameUrl}`,
      `EXPORTED: ${new Date().toISOString()}`,
      'EXPORT CREATED BY: RetroAchievements Badge + Logic Exporter userscript v3.2.0 (rawdanger-recomp)',
      'EXPORT NOTE: The userscript downloaded, renamed, indexed, and packaged all included images.',
      `USERSCRIPT FILE: ${SCRIPT_FILE}`,
      `OFFICIAL ACHIEVEMENTS: ${official.length}`,
      `UNOFFICIAL ACHIEVEMENTS: ${unofficial.length}`,
      `ICONS DOWNLOADED: ${variants.filter((v) => v.bytes).length} / ${variants.length}`,
      '',
      'COLOR IMAGE and GRAYSCALE IMAGE are the exact file paths inside this ZIP.',
      'AUTHOR is the RetroAchievements achievement creator, not necessarily the badge artist.',
      'A missing author is shown as "Not provided by page" rather than guessed.',
      'Identical achievement titles receive an ID suffix to prevent overwrites.',
      '',
    ];
    for (const [label, group] of [['OFFICIAL ACHIEVEMENTS', official], ['UNOFFICIAL ACHIEVEMENTS', unofficial]]) {
      lines.push(`==================== ${label} (${group.length}) ====================`, '');
      if (!group.length) lines.push('(None found)', '');
      for (let i = 0; i < group.length; i++) {
        const e = group[i];
        const [color, gray] = e.variants;
        lines.push(
          `${i + 1}. ${e.title}`,
          `TITLE: ${e.title}`,
          `AUTHOR: ${e.author}`,
          `DESCRIPTION: ${e.description}`,
          `COLOR IMAGE: ${color.path}`,
          `GRAYSCALE IMAGE: ${gray.path}`,
          `ACHIEVEMENT ID: ${e.id}`,
          `POINTS: ${e.points}`,
          `LOGIC: ${e.logic || '(not found)'}`,
          `COLOR STATUS: ${color.bytes ? 'Downloaded' : `FAILED - ${color.error}`}`,
          `GRAYSCALE STATUS: ${gray.bytes ? 'Downloaded' : `FAILED - ${gray.error}`}`,
          '',
        );
      }
    }
    return '\uFEFF' + lines.join('\r\n');
  }

  function escapeHtml(text) {
    return String(text).replace(/[&<>"']/g, (c) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
    })[c]);
  }

  function makeHtmlGallery(title, entries) {
    const rows = entries.map((e) => {
      const [color, gray] = e.variants;
      const images = [color, gray].map((variant) => `
        <figure>${variant.bytes ? `<img src="${escapeHtml(variant.path)}" alt="${escapeHtml(e.title)} - ${variant.label}">` : '<div class="no-image">Not downloaded</div>'}
          <figcaption>${escapeHtml(variant.label)}<small>${escapeHtml(variant.path)}</small></figcaption></figure>`).join('');
      return `<article class="achievement">
        <div class="images">${images}</div>
        <div class="details">
          <span class="type">${escapeHtml(e.category)} | ${escapeHtml(e.points)} points | ID ${escapeHtml(e.id)}</span>
          <h2>${escapeHtml(e.title)}</h2>
          <p>${escapeHtml(e.description)}</p>
          <p class="byline">Achievement author: <b>${escapeHtml(e.author)}</b></p>
        </div>
      </article>`;
    }).join('');
    return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${escapeHtml(title)} - Achievement Gallery</title>
<style>
:root{color-scheme:dark;font-family:system-ui,Segoe UI,sans-serif;background:#101215;color:#f2f2f2}
body{max-width:1100px;margin:25px auto;padding:0 18px}
h1{font-size:1.7rem;color:#edcc72}.summary{color:#adbac7}
.achievement{display:flex;gap:20px;align-items:center;padding:17px;margin:13px 0;background:#1d2229;border:1px solid #343b45;border-radius:12px}
.images{display:flex;gap:14px;flex-shrink:0}figure{margin:0;width:110px;text-align:center}
figure img,.no-image{width:96px;height:96px;object-fit:contain;image-rendering:pixelated;border-radius:6px;background:#111}
.no-image{display:grid;place-items:center;color:#ee7777;text-align:center;font-size:.8rem}
figcaption{color:#edcc72;font-size:.85rem;padding-top:4px}figcaption small{display:block;color:#91a4b5;font-size:.63rem;overflow-wrap:anywhere}
.details{min-width:0}h2{margin:5px 0 7px;font-size:1.1rem}p{margin:0 0 10px;line-height:1.4}
.type{font-size:.78rem;color:#e8c56b;text-transform:uppercase;letter-spacing:.04em}.byline{font-size:.85rem;color:#b7c6d2}
@media(max-width:650px){.achievement{display:block}.images{margin-bottom:12px}}
</style></head><body>
<h1>${escapeHtml(title)} - All Achievements</h1>
<p class="summary">${entries.filter((e) => e.category === 'Official').length} official + ${entries.filter((e) => e.category === 'Unofficial').length} unofficial achievements, showing downloaded color and grayscale badge images side by side. Extract the entire ZIP before opening this file.</p>
<p class="summary">Export generated by RetroAchievements Badge Exporter userscript v2.2.0. Installer: ${escapeHtml(SCRIPT_FILE)}. Authors indicate achievement creators, not necessarily badge artists.</p>
${rows}</body></html>`;
  }

  function downloadBlob(blob, filename) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
  }

  function setProgress(text, isError = false) {
    const status = document.getElementById(`${PANEL_ID}-status`);
    if (status) {
      status.textContent = text;
      status.style.color = isError ? '#ff8d8d' : '#d3dae2';
    }
  }

  async function exportAll() {
    if (exporting || !currentGameId()) return;
    exporting = true;
    const idAtStart = currentGameId();
    const button = document.getElementById(`${PANEL_ID}-button`);
    if (button) button.disabled = true;
    try {
      setProgress('Reading official + unofficial achievement metadata...');
      const coreUrl = gamePageUrl(false);
      const unofficialUrl = gamePageUrl(true);
      const [officialPage, unofficialPage] = await Promise.all([
        fetchGamePage(coreUrl), fetchGamePage(unofficialUrl),
      ]);
      const title = gameTitle(officialPage);
      const officialRaw = getAchievements(officialPage, false);
      const officialIDs = new Set(officialRaw.map((a) => String(a.id)));
      // Unpublished mode can include all achievements on some site versions;
      // subtract the official IDs to avoid exporting duplicate official badges.
      const unofficialRaw = getAchievements(unofficialPage, true)
        .filter((a) => !officialIDs.has(String(a.id)));
      const official = prepareEntries(officialRaw, 'Official');
      const unofficial = prepareEntries(unofficialRaw, 'Unofficial');
      const entries = [...official, ...unofficial];
      if (!entries.length) throw new Error('No achievements were found. Export canceled.');
      if (idAtStart !== currentGameId()) throw new Error('Game page changed while loading. Return to the game and retry.');

      const variants = entries.flatMap((e) => e.variants);
      setProgress(`Found ${official.length} official + ${unofficial.length} unofficial. Downloading color + grayscale...`);
      await downloadBadges(variants, (done, total) => setProgress(`Downloading images: ${done}/${total}`));
      const good = variants.filter((v) => v.bytes).length;
      setProgress(`Packaging ${good}/${variants.length} images + authors + TXT + gallery + userscript...`);

      setProgress('Reading each achievement\'s unlock logic and the code notes...');
      const logic = await collectLogic(idAtStart, officialPage, entries, (n) => setProgress(`Reading achievement pages for their logic: ${n}`));
      const files = variants.filter((v) => v.bytes).map((v) => ({ name: v.path, data: v.bytes }));
      files.push({ name: 'Achievements_Logic.txt', data: encoder.encode(makeLogicText(title, coreUrl, entries, logic)) });
      if (logic.found) files.push({ name: `RA_Set_${idAtStart}.json`, data: encoder.encode(makeOfflineSet(idAtStart, title, entries, logic)) });
      files.push({ name: 'Achievements.txt', data: encoder.encode(makeTextIndex(title, coreUrl, entries)) });
      files.push({ name: 'Gallery.html', data: encoder.encode(makeHtmlGallery(title, entries)) });
      const selfCopy = bundledScriptSource();
      if (selfCopy) files.push({ name: SCRIPT_FILE, data: encoder.encode(selfCopy) });
      files.push({ name: 'Export_Credits.txt', data: encoder.encode([
        'RetroAchievements Badge + Logic Exporter v3.2.0 (rawdanger-recomp, from the Badge Exporter v2.2.0)',
        '',
        'This archive was created by the RetroAchievements Badge Exporter userscript.',
        'The script downloaded original color and locked (grayscale) achievement badges,',
        'renamed them using achievement titles, and collected descriptions, creators, IDs and points.',
        'Official and unofficial achievement icons are stored in separate folders.',
        'Achievements.txt lists the precise color and grayscale filenames for each entry.',
        'Achievements_Logic.txt: each achievement\'s unlock logic (RetroAchievements syntax) and the same in words;',
        'RA_Set_<game>.json: the logic as an offline set for rawdanger-recomp (only when logic was found).',
        'Gallery.html displays both variants beside the description and achievement author.',
        '',
        'IMPORTANT: The author field identifies the creator of the achievement.',
        'It does not necessarily credit the person who drew the badge artwork.',
        'Unknown authors are labeled as such; no authors are invented.',
        '',
        'Bundled userscript: ' + SCRIPT_FILE,
        'Install it through Tampermonkey to run another export.',
        'If images are missing, see Failed_Downloads.txt and Achievements.txt.',
        '',
        'Original game page: ' + coreUrl,
        'Generated: ' + new Date().toISOString(),
      ].join('\r\n')) });
      if (good < variants.length) {
        files.push({
          name: 'Failed_Downloads.txt',
          data: encoder.encode(variants.filter((v) => !v.bytes)
            .map((v) => `${v.path}\r\nURL: ${v.url}\r\nERROR: ${v.error}`).join('\r\n\r\n')),
        });
      }
      const zip = zipArchive(files);
      downloadBlob(zip, `${cleanFileName(title, `Game ${idAtStart}`)} - Achievement Icons.zip`);
      setProgress(`ZIP created: ${good}/${variants.length} images, logic for ${logic.found}/${entries.length} (${logic.source}).` +
        (good < variants.length ? ' See Failed_Downloads.txt.' : ''));
    } catch (error) {
      console.error('[RA Badge Exporter]', error);
      setProgress(`Export failed: ${error.message || String(error)}`, true);
    } finally {
      exporting = false;
      if (button) button.disabled = false;
    }
  }

  function addStyle() {
    if (document.getElementById(STYLE_ID)) return;
    const style = document.createElement('style');
    style.id = STYLE_ID;
    style.textContent = `
      #${PANEL_ID}{position:fixed;right:16px;bottom:16px;z-index:2147483000;box-sizing:border-box;
      width:285px;max-width:calc(100vw - 32px);padding:14px;border-radius:12px;
      background:#191e25;color:#f2f2f2;border:1px solid #5d5342;box-shadow:0 5px 25px #0009;
      font:13px/1.4 system-ui,Segoe UI,sans-serif}
      #${PANEL_ID} strong{display:block;color:#f0ce7b;font-size:14px;margin-bottom:6px}
      #${PANEL_ID} button{width:100%;padding:10px 8px;margin:6px 0;border:0;border-radius:7px;
      background:#d6ad55;color:#111;font-weight:800;cursor:pointer}
      #${PANEL_ID} button:hover:not(:disabled){background:#efd084}
      #${PANEL_ID} button:disabled{cursor:wait;opacity:.55}
      #${PANEL_ID}-status{font-size:12px;display:block;overflow-wrap:anywhere}
      #${PANEL_ID} .subtitle{color:#b8c0c9;font-size:11px;margin-bottom:3px}`;
    document.head.appendChild(style);
  }

  function syncPanel() {
    const existing = document.getElementById(PANEL_ID);
    if (!currentGameId()) {
      if (existing && !exporting) existing.remove();
      return;
    }
    if (existing || !document.body || !document.head) return;
    addStyle();
    const panel = document.createElement('div');
    panel.id = PANEL_ID;
    const heading = document.createElement('strong');
    heading.textContent = 'RetroAchievements Badge + Logic Exporter';
    const subtitle = document.createElement('div');
    subtitle.className = 'subtitle';
    subtitle.textContent = 'Badges, texts and each achievement\'s unlock logic';
    const button = document.createElement('button');
    button.id = `${PANEL_ID}-button`;
    button.type = 'button';
    button.textContent = 'Download icons + logic (.ZIP)';
    button.addEventListener('click', exportAll);
    const status = document.createElement('span');
    status.id = `${PANEL_ID}-status`;
    status.textContent = 'Exports official + unofficial achievements with both icon variants.';
    panel.append(heading, subtitle, button, status);
    document.body.appendChild(panel);
  }

  syncPanel();
  // SPA navigation doesn't fire a full page reload. Keep the toolbar in sync.
  setInterval(syncPanel, 1500);
  window.addEventListener('popstate', syncPanel);
})();
