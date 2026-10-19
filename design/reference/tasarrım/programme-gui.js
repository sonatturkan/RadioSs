(() => {
    const workspace = document.querySelector('#flow-workspace');
    const rows = workspace?.querySelector('[data-programme-rows]');
    if (!rows) return;

    const labels = { music: 'Müzik', announcement: 'Anons', advert: 'Reklam', jingle: 'Jingle', programme: 'Program' };
    const titles = { music: 'Müzik bloğu', announcement: 'İstasyon anonsu', advert: 'Reklam kuşağı', jingle: 'İstasyon jingle', programme: 'Program bloğu' };
    const eventTitles = { music: 'Müzik eventi', announcement: 'Anons eventi', advert: 'Reklam eventi', jingle: 'Jingle eventi', programme: 'Program eventi' };
    const readinessLabels = { pending: 'Kontrol bekliyor', ready: 'Hazır', missing: 'Dosya eksik', approval: 'Onay bekliyor', invalid: 'Geçersiz reklam', locked: 'Kilitli' };
    const stageLabels = { draft: 'Taslak', review: 'Onay bekliyor', 'local-approved': 'Onaylı' };
    const storageKey = 'radios.programme-draft.v1';
    const copy = (value) => JSON.parse(JSON.stringify(value));
    const textValue = (value, max = 160) => typeof value === 'string' ? value.trim().slice(0, max) : '';
    const sourceMetadata = (source) => {
        const result = {};
        for (const key of ['sourceTrackId', 'location', 'sourceLocation', 'bpm', 'genre', 'year', 'rating', 'key', 'tonality', 'comment', 'cues', 'grid', 'tempo', 'rekordbox', 'automation']) {
            if (source[key] === undefined) continue;
            try { result[key] = copy(source[key]); } catch (_) { /* Ignore non-serializable transfer metadata. */ }
        }
        return result;
    };
    const validDuration = (value) => typeof value === 'string' && /^[0-9]{1,3}:[0-5][0-9]$/.test(value);
    const validTime = (value) => typeof value === 'string' && /^(?:[01][0-9]|2[0-3]):[0-5][0-9]$/.test(value);
    const seconds = (value) => { const [minutes, secs] = value.split(':').map(Number); return minutes * 60 + secs; };
    const timeSeconds = (value) => { const [hours, minutes] = value.split(':').map(Number); return hours * 3600 + minutes * 60; };
    const durationText = (value) => { const rounded = Math.round(Math.abs(value)); return `${Math.floor(rounded / 60)}:${String(rounded % 60).padStart(2, '0')}`; };
    const clockText = (value) => {
        value = Math.round(value);
        const days = Math.floor(value / 86400), hour = Math.floor(value / 3600) % 24, minute = Math.floor(value / 60) % 60, second = value % 60;
        return `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}:${String(second).padStart(2, '0')}${days ? ` (+${days} gün)` : ''}`;
    };
    let nextId = 0;
    const makeItem = (type, title = titles[type]) => ({ id: `programme-item-${++nextId}`, type, title, artist: '', album: '', time: '', duration: '', notes: '', version: 'v1', readiness: 'pending', primaryState: 'unknown', backupState: 'unknown', isBlock: false, sourceId: '' });
    const makeBlock = (type, title = titles[type], contents = []) => ({ ...makeItem(type, title), isBlock: true, contents });
    const sampleLeaf = (type, title, artist, album, duration) => ({ ...makeItem(type, title), artist, album, duration, notes: '', sourceId: `sample:${type}:${title}` });
    const defaults = [makeBlock('music'), makeBlock('announcement'), makeBlock('advert')];
    defaults.forEach((block) => { block.notes = ''; });
    defaults[0].contents = [sampleLeaf('music', 'Gün Doğarken', 'Ada Frekans', 'İlk Işık', '03:20'), sampleLeaf('music', 'Ufuk Çizgisi', 'Yankı Atölyesi', 'Mavi Saat', '03:54'), sampleLeaf('jingle', 'İstasyon jingle', 'Radio S', '', '00:08')];
    defaults[1].contents = [sampleLeaf('announcement', 'İstasyon anonsu', 'Radio S', '', '00:12')];
    defaults[2].contents = [sampleLeaf('jingle', 'Reklam kuşağı giriş jingle', 'Radio S', '', '00:05'), sampleLeaf('advert', 'Tanıtım spotu', '', '', '00:30'), sampleLeaf('advert', 'Etkinlik spotu', '', '', '00:20')];
    let programme = { name: 'Örnek program', date: '', notes: '', items: defaults, revision: 1, stage: 'draft', timing: { title: 'Kesin saatli olay', start: '', target: '', policy: 'cut', maxWait: 30 }, transitions: [] };
    let savedRevision = 0, savedAt = '', restored = false, restoredSelection = null, restoredExpanded = [];
    const markerNumber = (value, fallback, min, max) => {
        if (!['number', 'string'].includes(typeof value) || typeof value === 'string' && !value.trim() || !Number.isFinite(Number(value))) return fallback;
        const bounded = Math.max(min, Math.min(max, Number(value)));
        const rounded = Math.round(bounded * 1000) / 1000;
        return Number.isFinite(rounded) ? rounded : bounded;
    };
    const normalizeMarkers = (input, item) => {
        if (!input || typeof input !== 'object') return undefined;
        if (!['cueIn', 'intro', 'out', 'cueOut', 'fadeStart', 'fadeEnd', 'autoNext', 'overlap', 'automatic'].some((key) => Object.hasOwn(input, key))) return undefined;
        const limit = validDuration(item.duration) ? seconds(item.duration) : Infinity;
        const cueIn = markerNumber(input.cueIn, 0, 0, limit);
        const hasOut = Object.hasOwn(input, 'out') || Object.hasOwn(input, 'cueOut');
        const outInput = Object.hasOwn(input, 'out') ? input.out : input.cueOut;
        const out = markerNumber(outInput, hasOut ? null : Number.isFinite(limit) ? limit : null, cueIn, limit);
        const end = out ?? limit;
        const fadeStart = markerNumber(input.fadeStart, null, cueIn, end);
        return { cueIn, intro: markerNumber(input.intro, null, cueIn, end), out, fadeStart, fadeEnd: markerNumber(input.fadeEnd, null, fadeStart ?? cueIn, end), autoNext: markerNumber(input.autoNext, null, cueIn, end), overlap: markerNumber(input.overlap, 0, 0, Number.isFinite(end) ? Math.max(0, end - cueIn) : Infinity), automatic: input.automatic === true, ...(textValue(input.nextId, 100) ? { nextId: textValue(input.nextId, 100) } : {}) };
    };
    const eachItem = (items, callback, parent = null) => items.forEach((item, index) => { callback(item, parent, items, index); if (item.isBlock && Array.isArray(item.contents)) eachItem(item.contents, callback, item); });
    const findRef = (id) => {
        let found = null;
        eachItem(programme.items, (item, parent, container, index) => { if (item.id === id) found = { item, parent, container, index }; });
        return found;
    };
    const leavesOf = (item) => item.isBlock ? item.contents.flatMap(leavesOf) : [item];
    const hasLockedContents = (item) => item.readiness === 'locked' || item.isBlock && item.contents.some(hasLockedContents);
    const leafRefs = () => {
        const result = [];
        eachItem(programme.items, (item, parent) => { if (!item.isBlock) result.push({ item, parent }); });
        return result;
    };
    const playbackItems = () => leafRefs().map(({ item, parent }) => ({ ...copy(item), parentId: parent?.id || null }));
    const normalizedIds = new Set();
    const scanIds = (items, depth = 0) => {
        if (!Array.isArray(items) || depth > 12) return;
        items.forEach((entry) => {
            if (!entry || typeof entry !== 'object') return;
            const match = /^programme-item-([1-9][0-9]{0,8})$/.exec(entry.id);
            if (match) nextId = Math.max(nextId, Number(match[1]));
            scanIds(entry.contents, depth + 1);
        });
    };
    const normalizeEntry = (entry, depth = 0, top = false) => {
        if (depth > 12) return [];
        if (Array.isArray(entry)) return entry.flatMap((child) => normalizeEntry(child, depth + 1, top));
        if (!entry || typeof entry !== 'object' || depth > 12 || !Object.hasOwn(labels, entry.type) || !textValue(entry.title)) return [];
        const genericBlock = top && entry.isBlock !== false && [titles.music, titles.announcement, titles.advert, 'Anons kuşağı'].includes(entry.title);
        const isBlock = entry.isBlock === true || Array.isArray(entry.contents) || genericBlock || entry.type === 'programme';
        const item = isBlock ? makeBlock(entry.type, textValue(entry.title)) : makeItem(entry.type, textValue(entry.title));
        if (typeof entry.id === 'string' && /^[a-zA-Z0-9_-]{1,100}$/.test(entry.id) && !normalizedIds.has(entry.id)) item.id = entry.id;
        while (normalizedIds.has(item.id)) item.id = `programme-item-${++nextId}`;
        normalizedIds.add(item.id);
        for (const key of ['artist', 'album']) item[key] = textValue(entry[key], 120);
        item.notes = textValue(entry.notes, 300); item.version = textValue(entry.version, 32) || 'v1';
        item.duration = validDuration(entry.duration) ? entry.duration : ''; item.time = validTime(entry.time) ? entry.time : '';
        item.sourceId = textValue(entry.sourceId, 160);
        Object.assign(item, sourceMetadata(entry));
        item.readiness = Object.hasOwn(readinessLabels, entry.readiness) ? entry.readiness : 'pending';
        for (const key of ['primaryState', 'backupState']) item[key] = ['unknown', 'present', 'missing'].includes(entry[key]) ? entry[key] : 'unknown';
        if (item.readiness === 'ready' && (item.primaryState !== 'present' || item.backupState !== 'present')) item.readiness = 'pending';
        if (isBlock) {
            item.declaredDuration = validDuration(entry.declaredDuration) ? entry.declaredDuration : item.duration;
            item.contents = Array.isArray(entry.contents) ? entry.contents.flatMap((child) => normalizeEntry(child, depth + 1, false)) : [];
        } else {
            const markers = normalizeMarkers(entry.markers, item);
            if (markers) item.markers = markers;
        }
        return [item];
    };
    try {
        const stored = JSON.parse(localStorage.getItem(storageKey) || 'null');
        const source = stored?.programme;
        if (source && Array.isArray(source.items) && textValue(source.name, 80)) {
            scanIds(source.items);
            const items = source.items.flatMap((entry) => normalizeEntry(entry, 0, true));
            const timing = source.timing || {};
            programme = { name: textValue(source.name, 80), date: /^\d{4}-\d{2}-\d{2}$/.test(source.date) && Number.isFinite(new Date(`${source.date}T00:00:00Z`).getTime()) ? source.date : '', notes: textValue(source.notes, 400), items,
                transitions: Array.isArray(source.transitions) ? source.transitions.filter((transition) => transition && normalizedIds.has(transition.fromId) && normalizedIds.has(transition.toId) && Number.isFinite(transition.overlap) && transition.overlap >= 0).map((transition) => ({ fromId: transition.fromId, toId: transition.toId, overlap: Math.min(3600, transition.overlap) })) : [],
                revision: Number.isSafeInteger(source.revision) && source.revision > 0 ? source.revision : 1, stage: Object.hasOwn(stageLabels, source.stage) ? source.stage : 'draft',
                timing: { title: textValue(timing.title, 80) || 'Kesin saatli olay', start: validTime(timing.start) ? timing.start : '', target: validTime(timing.target) ? timing.target : '', policy: ['cut', 'finish'].includes(timing.policy) ? timing.policy : 'cut', maxWait: Number.isInteger(timing.maxWait) ? Math.max(0, Math.min(600, timing.maxWait)) : 30 } };
            savedRevision = programme.revision; savedAt = textValue(stored.savedAt, 40); restored = true;
            restoredSelection = textValue(stored.selectedId, 100);
            restoredExpanded = Array.isArray(stored.expandedBlocks) ? stored.expandedBlocks.filter((id) => normalizedIds.has(id)) : [];
        }
    } catch { /* Local storage is optional. A damaged draft does not prevent editing. */ }
    let selectedId = findRef(restoredSelection)?.item.id || programme.items[0]?.id || null;
    const expanded = new Set(restoredExpanded);
    const ensureRootEvents = () => {
        programme.items = programme.items.map((item) => {
            if (item.isBlock) return item;
            const event = makeBlock(item.type, eventTitles[item.type], [item]);
            expanded.add(event.id);
            return event;
        });
    };
    ensureRootEvents();
    let queueMode = 'insert';
    let guiPlayback = { itemId: null, state: 'idle' };
    const selectedRef = () => findRef(selectedId);
    const selectedItem = () => selectedRef()?.item;
    const transportIndex = () => {
        const sequence = leafRefs();
        const activeId = guiPlayback.itemId || (selectedItem() ? leavesOf(selectedItem())[0]?.id : null);
        return sequence.findIndex(({ item }) => item.id === activeId);
    };
    const activeBlock = () => { const ref = selectedRef(); return ref?.item.isBlock ? ref.item : ref?.parent || null; };
    const effectiveSeconds = (item) => validDuration(item.duration) ? item.markers ? Math.max(0, (item.markers.out ?? seconds(item.duration)) - (item.markers.cueIn ?? 0)) : seconds(item.duration) : null;
    const savedOverlap = (from, to) => {
        if (!to) return 0;
        const transition = programme.transitions.find((entry) => entry.fromId === from.id && entry.toId === to.id);
        if (!transition) return 0;
        return Math.min(transition.overlap, effectiveSeconds(from) ?? 0, effectiveSeconds(to) ?? 0);
    };
    const refreshBlockDurations = () => {
        eachItem(programme.items, (item) => {
            if (!item.isBlock) return;
            const leaves = leavesOf(item);
            if (!leaves.length || leaves.some((leaf) => effectiveSeconds(leaf) === null)) { item.duration = ''; return; }
            const total = leaves.reduce((sum, leaf, index) => sum + effectiveSeconds(leaf) - savedOverlap(leaf, leaves[index + 1]), 0);
            item.duration = durationText(Math.max(0, total));
        });
    };
    const pruneTransitions = () => {
        const refs = leafRefs();
        programme.transitions = programme.transitions.filter((entry) => refs.some(({ item }, index) => item.id === entry.fromId && refs[index + 1]?.item.id === entry.toId));
    };
    const changed = () => {
        ensureRootEvents();
        programme.revision += 1; programme.stage = 'draft'; pruneTransitions(); refreshBlockDurations();
        if (!findRef(guiPlayback.itemId)) guiPlayback = { itemId: null, state: 'idle' };
    };
    const timingAnalysis = () => {
        const items = leafRefs().map(({ item }) => item);
        const unknownCount = items.filter((item) => effectiveSeconds(item) === null).length;
        const knownTotal = items.reduce((sum, item) => sum + (effectiveSeconds(item) ?? 0), 0);
        const overlapSeconds = items.reduce((sum, item, index) => sum + savedOverlap(item, items[index + 1]), 0);
        const total = Math.max(0, knownTotal - overlapSeconds);
        const result = { unknownCount, knownSeconds: knownTotal, overlapSeconds, totalSeconds: total, estimate: null, delta: null, leafCount: items.length, blockCount: programme.items.filter((item) => item.isBlock).length };
        if (unknownCount || !items.length || !validTime(programme.timing.start)) return result;
        result.estimate = timeSeconds(programme.timing.start) + total;
        if (validTime(programme.timing.target)) { let target = timeSeconds(programme.timing.target); if (target < timeSeconds(programme.timing.start)) target += 86400; result.delta = result.estimate - target; }
        return result;
    };
    const snapshot = () => {
        refreshBlockDurations();
        const ref = selectedRef();
        return { programme: copy(programme), selectedItem: ref ? { ...copy(ref.item), parentId: ref.parent?.id || null } : null, selectedId, selectedBlockId: activeBlock()?.id || null, selectedLeafId: ref && !ref.item.isBlock ? ref.item.id : null, playbackItems: playbackItems(), timingAnalysis: timingAnalysis(), savedRevision, savedAt, guiPlayback: { ...guiPlayback }, queueMode };
    };
    const requestWaveform = (item) => {
        if (!item || item.isBlock) return;
        const items = playbackItems(), index = items.findIndex((leaf) => leaf.id === item.id);
        document.dispatchEvent(new CustomEvent('radios:programme-waveform-request', { detail: { item: copy(items[index]), nextItem: items[index + 1] ? copy(items[index + 1]) : null, blockId: findRef(item.id)?.parent?.id || null } }));
    };
    const selectItem = (id, options = {}) => {
        const ref = findRef(id);
        if (!ref) return false;
        selectedId = id;
        if (ref.item.isBlock) expanded.add(ref.item.id);
        for (let parent = ref.parent; parent; parent = findRef(parent.id)?.parent) expanded.add(parent.id);
        render();
        if (options.openWaveform && !ref.item.isBlock) requestWaveform(ref.item);
        return true;
    };
    window.RadioSProgramme = Object.freeze({ getSnapshot: snapshot, getEditableSequence: playbackItems, selectItem });
    const setText = (selector, value) => {
        const element = workspace.querySelector(selector);
        if (element) element.textContent = value;
    };
    const announce = (message) => setText('[data-programme-status]', message);
    const element = (tag, className, text) => {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (text !== undefined) node.textContent = text;
        return node;
    };

    const overlapText = (value) => `${(Math.round(value * 10) / 10).toLocaleString('tr-TR')} sn`;
    // The mix editor opens beside the transition it changes (Rapor s.2, MIKS-04).
    const transitionRow = (from, to) => {
        const row = element('tr', 'ld-transition-row');
        row.dataset.programmeTransitionRow = from.id;
        const cell = element('td');
        cell.colSpan = 4;
        const saved = programme.transitions.find((entry) => entry.fromId === from.id && entry.toId === to.id);
        const gap = element('div', `ld-gap${saved ? ' is-set' : ''}`);
        gap.append(element('span', '', saved ? `Geçiş · ${overlapText(saved.overlap)} bindirme` : 'Geçiş ayarlanmadı'));
        const edit = element('button', 'ld-gap-btn', 'Geçişi düzenle');
        edit.type = 'button';
        edit.dataset.programmeTransition = from.id;
        edit.setAttribute('aria-label', `${from.title} → ${to.title} geçişini düzenle`);
        gap.append(edit);
        cell.append(gap);
        row.append(cell);
        return row;
    };

    const render = () => {
        refreshBlockDurations();
        const focused = rows.contains(document.activeElement) ? { ...document.activeElement.dataset } : {};
        const fragment = document.createDocumentFragment();
        const sequence = leafRefs().map(({ item }) => item);
        const renderItem = (item, path, parent = null) => {
            const active = item.id === selectedId;
            const row = element('tr', `ld-row ${item.isBlock ? 'programme-block-row' : parent ? 'programme-child-row' : 'programme-leaf-row'}${active ? ' is-selected' : ''}${item.isBlock && expanded.has(item.id) ? ' is-expanded' : ''}`);
            row.dataset.programmeItem = item.id;
            if (item.isBlock) row.dataset.programmeEvent = item.id;
            if (!item.isBlock) row.dataset.programmeLeaf = item.id;
            if (parent) row.dataset.programmeParent = parent.id;
            row.draggable = item.readiness !== 'locked';
            const numberCell = element('td', 'broadcast-queue-number');
            const number = element('button', item.isBlock ? 'programme-block-number broadcast-queue-index' : 'programme-leaf-number broadcast-queue-index', path);
            number.type = 'button';
            if (item.isBlock) {
                number.dataset.programmeBlockToggle = item.id;
                number.setAttribute('aria-expanded', String(expanded.has(item.id)));
                number.setAttribute('aria-label', `${item.title} eventi: içeriği ${expanded.has(item.id) ? 'kapat' : 'aç'}`);
            } else {
                number.dataset.selectProgrammeLeaf = item.id;
                number.setAttribute('aria-label', `${item.title}: cue ve geçişlerini düzenle`);
            }
            numberCell.append(number, element('small', 'broadcast-queue-time', item.time || '—:—'));
            row.append(numberCell);
            const contentCell = element('td');
            const select = element('button', 'ld-item');
            select.type = 'button';
            if (item.isBlock) {
                select.dataset.selectProgrammeItem = item.id;
                select.setAttribute('aria-expanded', String(expanded.has(item.id)));
                select.setAttribute('aria-label', `${item.title} eventi: içindeki parçaları göster`);
            }
            else select.dataset.selectProgrammeLeaf = item.id;
            select.setAttribute('aria-pressed', String(active));
            select.append(element('span', 'broadcast-queue-artist', item.isBlock ? `Event · ${labels[item.type]}` : item.artist || labels[item.type]), element('span', 'ld-item-title', item.title));
            select.append(element('span', 'ld-item-meta', [item.album, labels[item.type], item.version, item.isBlock ? `${leavesOf(item).length} parça / içerik` : 'Cue · geçiş'].filter(Boolean).join(' · ')));
            contentCell.append(select);
            row.append(contentCell, element('td', 'ld-num broadcast-queue-duration', item.duration || '—:—'));
            const actions = element('div', 'broadcast-row-actions');
            actions.setAttribute('role', 'group');
            actions.setAttribute('aria-label', `${item.title} öğesi kontrolleri`);
            const addAction = (action, symbol, label) => {
                const button = element('button', '', symbol);
                button.type = 'button'; button.dataset.programmeRowAction = action; button.dataset.programmeRowItem = item.id;
                button.setAttribute('aria-label', `${item.title}: ${label}`); button.title = `${item.title}: ${label}`;
                if (action === 'remove' && hasLockedContents(item)) button.disabled = true;
                if (action === 'play') {
                    const leaf = item.isBlock ? leavesOf(item)[0] : item;
                    button.disabled = !leaf;
                    button.setAttribute('aria-pressed', String(Boolean(leaf && guiPlayback.itemId === leaf.id && guiPlayback.state === 'started')));
                }
                actions.append(button);
            };
            addAction('play', 'S', 'Başlat');
            addAction('waveform', 'V', item.isBlock ? 'Event içeriğini göster' : 'Cue ve geçişlerini düzenle');
            addAction('properties', 'Ö', 'Özellikleri düzenle');
            addAction('remove', '×', 'Yayın sırasından kaldır');
            const stateCell = element('td', 'broadcast-queue-state');
            const state = element('span', `ld-state ld-state--${item.readiness}`, !item.isBlock && guiPlayback.itemId === item.id && guiPlayback.state !== 'idle' ? (guiPlayback.state === 'paused' ? 'Duraklatıldı' : 'Başlatıldı') : readinessLabels[item.readiness]);
            stateCell.append(actions, state); row.append(stateCell); fragment.append(row);
            if (item.isBlock && expanded.has(item.id)) {
                if (!item.contents.length) {
                    const emptyRow = element('tr', 'programme-empty-block-row'); emptyRow.dataset.programmeParent = item.id;
                    const cell = element('td', 'ld-empty-cell', 'Boş event'); cell.colSpan = 4;
                    emptyRow.append(cell); fragment.append(emptyRow);
                } else item.contents.forEach((child, index) => renderItem(child, `${path}.${index + 1}`, item));
            } else if (!item.isBlock) {
                const index = sequence.findIndex((leaf) => leaf.id === item.id), next = sequence[index + 1];
                if (next) fragment.append(transitionRow(item, next));
            }
        };
        programme.items.forEach((item, index) => renderItem(item, String(index + 1)));
        if (!programme.items.length) {
            const row = element('tr'), cell = element('td', 'ld-empty-cell', 'Event yok'); cell.colSpan = 4; row.append(cell); fragment.append(row);
        }
        rows.replaceChildren(fragment);
        const focusKeys = ['programmeBlockToggle', 'selectProgrammeLeaf', 'selectProgrammeItem', 'programmeTransition'];
        const focusKey = focusKeys.find((key) => focused[key]);
        if (focusKey) {
            const attribute = `data-${focusKey.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)}`;
            rows.querySelector(`[${attribute}="${CSS.escape(focused[focusKey])}"]`)?.focus({ preventScroll: true });
        } else if (focused.programmeRowItem && focused.programmeRowAction) rows.querySelector(`[data-programme-row-item="${CSS.escape(focused.programmeRowItem)}"][data-programme-row-action="${CSS.escape(focused.programmeRowAction)}"]`)?.focus({ preventScroll: true });
        setText('[data-programme-count]', `${programme.items.length} event · ${sequence.length} içerik`);
        setText('[data-programme-selection-name]', selectedItem()?.title || 'Öğe seçilmedi');
        setText('[data-programme-title]', programme.name);
        setText('[data-programme-date]', programme.date
            ? new Intl.DateTimeFormat('tr-TR', { dateStyle: 'long', timeZone: 'UTC' }).format(new Date(`${programme.date}T00:00:00Z`))
            : 'Tarih seçilmedi');

        const ref = selectedRef(), index = ref?.index ?? -1;
        for (const button of document.querySelectorAll('[data-programme-action]')) {
            const action = button.dataset.programmeAction;
            button.disabled = index < 0 || selectedItem()?.readiness === 'locked' || (action === 'up' && index === 0) || (action === 'down' && index === ref.container.length - 1);
        }
        for (const button of document.querySelectorAll('[data-open-programme-properties]')) button.disabled = index < 0;
        workspace.querySelectorAll('[data-programme-queue-action]').forEach((button) => {
            const action = button.dataset.programmeQueueAction;
            button.disabled = ['up', 'down', 'top', 'bottom', 'replace'].includes(action) && (!ref || ref.item.readiness === 'locked')
                || action === 'replace' && Boolean(ref?.item.isBlock)
                || action === 'clear' && !programme.items.length
                || action === 'play' && (!ref || !leavesOf(ref.item).length)
                || action === 'previous' && transportIndex() <= 0
                || action === 'next' && (transportIndex() < 0 || transportIndex() >= leafRefs().length - 1)
                || action === 'pause' && guiPlayback.state !== 'started'
                || action === 'stop' && guiPlayback.state === 'idle';
            if (action === 'play' || action === 'pause') button.setAttribute('aria-pressed', String(guiPlayback.state === (action === 'play' ? 'started' : 'paused')));
        });
        setText('[data-programme-placement]', `${activeBlock() ? `Event: ${activeBlock().title} · ` : ''}${queueMode === 'replace' ? 'Seçili içeriği değiştir' : queueMode === 'append' ? 'Event sonuna ekle' : 'Seçili içeriğin sonrasına ekle'}`);
        setText('[data-programme-revision]', `Revizyon ${programme.revision}`);
        setText('[data-programme-stage]', stageLabels[programme.stage]);
        const stage = workspace.querySelector('[data-programme-stage]');
        if (stage) stage.dataset.stage = programme.stage;
        setText('[data-programme-save-state]', savedRevision === programme.revision ? 'Tarayıcıda kaydedildi' : 'Kaydedilmemiş değişiklikler');
        workspace.querySelectorAll('[data-programme-send-review]').forEach((button) => { button.disabled = !programme.items.length || programme.stage === 'local-approved'; });
        const analysis = timingAnalysis();
        setText('[data-programme-timed-title]', programme.timing.title);
        setText('[data-programme-target-time]', programme.timing.target || '—:—');
        setText('[data-programme-estimated-time]', analysis.estimate === null ? 'Belirsiz' : clockText(analysis.estimate));
        setText('[data-programme-time-delta]', analysis.delta === null ? 'Hesaplanamıyor' : analysis.delta === 0 ? 'Tam zamanında' : `${durationText(analysis.delta)} ${analysis.delta > 0 ? 'geç' : 'erken'}`);
        const delta = workspace.querySelector('[data-programme-time-delta]');
        if (delta) delta.dataset.direction = analysis.delta === null ? 'unknown' : analysis.delta > 0 ? 'late' : analysis.delta < 0 ? 'early' : 'ontime';
        setText('[data-programme-timing-policy]', programme.timing.policy === 'cut' ? 'Hedef saatte kes' : `Bitmesini bekle · en çok ${programme.timing.maxWait} sn`);
        setText('[data-programme-timing-note]', analysis.unknownCount ? `Süresi eksik: ${analysis.unknownCount} öğe` : programme.timing.start && analysis.overlapSeconds ? `Bindirme −${durationText(analysis.overlapSeconds)}` : '');
        document.dispatchEvent(new CustomEvent('radios:programme-change', { detail: snapshot() }));
    };

    const field = (form, name) => form?.elements.namedItem(name);
    const setField = (form, name, value) => {
        const input = field(form, name);
        if (input) {
            input.value = value;
            if (typeof input.setCustomValidity === 'function') input.setCustomValidity('');
        }
    };
    const readField = (form, name) => String(field(form, name)?.value || '').trim();
    const openEditor = (id, values) => {
        const dialog = document.getElementById(id);
        const form = dialog?.querySelector('form');
        if (!dialog || typeof dialog.showModal !== 'function') return;
        for (const [name, value] of Object.entries(values)) setField(form, name, value);
        if (id === 'programme-item-editor') {
            form.dataset.programmeEditingId = values.id;
            const duration = field(form, 'duration'); if (duration) duration.disabled = values.isBlock === true;
        }
        if (!dialog.open) dialog.showModal();
    };
    const closeDialog = (dialog) => {
        if (dialog?.open && typeof dialog.close === 'function') dialog.close();
    };

    const transferType = 'application/x-radios-programme-items';
    const internalTransferType = 'application/x-radios-programme-row';
    const isLocked = (ref) => { for (let current = ref; current; current = current.parent ? findRef(current.parent.id) : null) if (current.item.readiness === 'locked') return true; return false; };
    const removeItem = (id) => {
        const ref = findRef(id);
        if (!ref || isLocked(ref) || hasLockedContents(ref.item)) { announce('Kilitli içerik değiştirilemez. Özelliklerden kilidi kaldır.'); return false; }
        ref.container.splice(ref.index, 1);
        expanded.delete(id);
        selectedId = ref.container[Math.min(ref.index, ref.container.length - 1)]?.id || ref.parent?.id || programme.items[0]?.id || null;
        changed(); render(); announce(`${ref.item.title} kaldırıldı.`); return true;
    };
    const moveItem = (id, destination, parentId = undefined) => {
        const ref = findRef(id);
        if (!ref || isLocked(ref) || hasLockedContents(ref.item)) return false;
        const destinationParent = parentId === undefined ? ref.parent : parentId ? findRef(parentId)?.item : null;
        if (parentId && !destinationParent?.isBlock) return false;
        if (destinationParent && (destinationParent.id === id || leavesOf(ref.item).some((leaf) => leaf.id === destinationParent.id))) return false;
        if (destinationParent && isLocked(findRef(destinationParent.id))) return false;
        // A block cannot move inside any of its descendant blocks.
        for (let ancestor = destinationParent; ancestor; ancestor = findRef(ancestor.id)?.parent) if (ancestor.id === id) return false;
        const container = destinationParent ? destinationParent.contents : programme.items;
        let position = Math.max(0, Math.min(container.length, destination));
        if (container === ref.container && position > ref.index) position -= 1;
        if (container === ref.container && position === ref.index) return false;
        ref.container.splice(ref.index, 1); container.splice(position, 0, ref.item);
        selectedId = ref.item.id; if (destinationParent) expanded.add(destinationParent.id);
        changed(); render(); announce(`${ref.item.title} taşındı.`); return true;
    };
    const runMove = (action) => {
        const ref = selectedRef(); if (!ref || isLocked(ref)) return;
        const position = action === 'top' ? 0 : action === 'bottom' ? ref.container.length : action === 'up' ? Math.max(0, ref.index - 1) : Math.min(ref.container.length, ref.index + 2);
        moveItem(ref.item.id, position);
    };
    const startGui = (item) => {
        const contents = item ? leavesOf(item) : [];
        const leaf = guiPlayback.state === 'paused' ? contents.find((entry) => entry.id === guiPlayback.itemId) || contents[0] : contents[0];
        if (!leaf) { announce('Event boş.'); return; }
        selectedId = leaf.id; if (findRef(leaf.id)?.parent) expanded.add(findRef(leaf.id).parent.id);
        guiPlayback = { itemId: leaf.id, state: 'started' }; render(); announce(`${leaf.title} başlatıldı.`);
    };
    const stepGui = (direction) => {
        const sequence = leafRefs(), index = transportIndex();
        const target = index < 0 ? null : sequence[index + direction];
        if (!target) return;
        selectedId = target.item.id;
        if (target.parent) expanded.add(target.parent.id);
        if (guiPlayback.state !== 'idle') guiPlayback.itemId = target.item.id;
        render(); announce(`${direction > 0 ? 'Sonraki' : 'Önceki'} içerik: ${target.item.title}`);
    };
    const requestLibrary = (mode) => {
        queueMode = mode;
        render();
        document.dispatchEvent(new CustomEvent('radios:programme-library-request', { detail: { mode, blockId: activeBlock()?.id || null, leafId: selectedRef() && !selectedRef().item.isBlock ? selectedId : null } }));
    };
    const copiedSources = (sources, depth = 0, seen = new WeakSet()) => {
        if (!Array.isArray(sources) || depth > 12 || seen.has(sources)) return [];
        seen.add(sources);
        return sources.flatMap((source) => {
            if (Array.isArray(source)) return copiedSources(source, depth + 1, seen);
            if (!source || typeof source !== 'object') return [];
            if (Array.isArray(source.contents) || Array.isArray(source.items)) return copiedSources(source.contents || source.items, depth + 1, seen);
            const title = textValue(source.title), type = source.type || 'music';
            if (!title || !Object.hasOwn(labels, type) || type === 'programme') return [];
            const item = makeItem(type, title);
            item.artist = textValue(source.artist, 120); item.album = textValue(source.album, 120); item.notes = textValue(source.notes, 300);
            item.sourceId = textValue(source.sourceId || source.id, 160); item.version = textValue(source.version, 32) || 'v1';
            Object.assign(item, sourceMetadata(source));
            item.duration = validDuration(source.duration) ? source.duration : ''; item.time = validTime(source.time) ? source.time : '';
            item.readiness = Object.hasOwn(readinessLabels, source.readiness) ? source.readiness : 'pending';
            for (const key of ['primaryState', 'backupState']) item[key] = ['unknown', 'present', 'missing'].includes(source[key]) ? source[key] : 'unknown';
            if (item.readiness === 'ready' && (item.primaryState !== 'present' || item.backupState !== 'present')) item.readiness = 'pending';
            const markers = normalizeMarkers(source.markers, item); if (markers) item.markers = markers;
            return [item];
        });
    };
    const addTransferredItems = (detail, placement = null) => {
        const imported = copiedSources(detail?.items);
        if (!imported.length) { announce('Aktarılacak geçerli içerik bulunamadı.'); return false; }
        const kind = detail?.kind === 'programme' ? 'programme' : 'items';
        const mode = ['insert', 'replace', 'append'].includes(detail?.mode) ? detail.mode : queueMode;
        if (kind === 'programme') {
            const group = makeBlock('programme', textValue(detail.programmeName || detail.playlistName) || 'Program bloğu', imported);
            group.sourceId = textValue(detail.sourceProgrammeId || detail.sourceId, 160);
            group.notes = textValue(detail.notes || detail.sourceLabel || detail.playlistName, 300);
            group.version = textValue(detail.version, 32) || 'v1';
            let position = placement && !placement.parentId ? placement.index : programme.items.length;
            if (!placement && mode !== 'append') { const top = selectedRef(); let rootItem = top?.item; for (let parent = top?.parent; parent; parent = findRef(parent.id)?.parent) rootItem = parent; if (rootItem) position = programme.items.indexOf(rootItem) + 1; }
            programme.items.splice(Math.max(0, Math.min(programme.items.length, position)), 0, group); selectedId = group.id; expanded.add(group.id);
        } else {
            let parent = placement ? placement.parentId ? findRef(placement.parentId)?.item : null : detail.targetBlockId ? findRef(detail.targetBlockId)?.item : activeBlock();
            if (!placement && detail.targetBlockId && !parent?.isBlock) { announce('Hedef blok bulunamadı.'); return false; }
            if (placement) parent = placement.parentId ? findRef(placement.parentId)?.item : null;
            const ref = selectedRef();
            if (parent && isLocked(findRef(parent.id)) || !parent && ref && isLocked(ref)) { announce('Kilitli hedefe içerik eklenemez.'); return false; }
            let container = parent?.contents || programme.items;
            let position = placement ? placement.index : mode === 'append' ? container.length : ref?.container === container ? ref.index + 1 : container.length;
            if (mode === 'replace' && !placement) {
                if (!ref || ref.item.isBlock || isLocked(ref)) { announce('Değiştirilecek parça seçili değil.'); return false; }
                container = ref.container; parent = ref.parent; position = ref.index;
                container.splice(ref.index, 1);
            }
            if (!parent) {
                const type = imported.every((item) => item.type === imported[0].type) ? imported[0].type : 'programme';
                const event = makeBlock(type, textValue(detail.eventName || detail.playlistName) || eventTitles[type], imported);
                container.splice(Math.max(0, Math.min(container.length, position)), 0, event);
                selectedId = event.id; expanded.add(event.id);
            } else {
                container.splice(Math.max(0, Math.min(container.length, position)), 0, ...imported);
                selectedId = imported[0].id; expanded.add(parent.id);
            }
        }
        queueMode = 'insert'; changed(); render();
        setTimeout(() => workspace.querySelector('.workspace-tab')?.click(), 0);
        announce(`${textValue(detail.playlistName, 120) ? `${textValue(detail.playlistName, 120)}: ` : ''}${imported.length} içerik ${kind === 'programme' ? 'program bloğuna' : 'yayın sırasına'} eklendi.`);
        return true;
    };
    document.addEventListener('radios:add-programme-items', (event) => addTransferredItems(event.detail));
    const saveOverlap = (fromId, toId, overlap) => {
        const index = leafRefs().findIndex(({ item }) => item.id === fromId);
        const sequence = leafRefs();
        if (index < 0 || sequence[index + 1]?.item.id !== toId) return false;
        const saved = { fromId, toId, overlap: Math.min(3600, overlap) };
        const existing = programme.transitions.findIndex((entry) => entry.fromId === fromId && entry.toId === toId);
        if (existing >= 0) programme.transitions[existing] = saved; else programme.transitions.push(saved);
        return true;
    };
    document.addEventListener('radios:transition-saved', (event) => {
        const transition = event.detail;
        if (!transition || !Number.isFinite(transition.overlap) || transition.overlap < 0 || !saveOverlap(transition.fromId, transition.toId, transition.overlap)) return;
        changed(); render(); announce('Bindirme uygulandı.');
    });
    document.addEventListener('radios:programme-markers-saved', (event) => {
        const detail = event.detail, ref = findRef(detail?.itemId);
        if (!ref || ref.item.isBlock || isLocked(ref)) return;
        const markers = normalizeMarkers(detail.markers, ref.item); if (!markers) return;
        const sequence = leafRefs(), index = sequence.findIndex(({ item }) => item.id === ref.item.id), next = sequence[index + 1]?.item;
        if (detail.nextId && next?.id !== detail.nextId) { announce('Sıradaki içerik değişti. Geçişi yeniden açıp kaydet.'); return; }
        if (next && detail.nextId === next.id) {
            markers.nextId = next.id;
            saveOverlap(ref.item.id, next.id, markers.automatic ? markers.overlap : 0);
        } else {
            markers.overlap = 0; delete markers.nextId;
            if (next && !markers.automatic) saveOverlap(ref.item.id, next.id, 0);
        }
        ref.item.markers = markers; changed(); render(); announce(`${ref.item.title}: işaretler kaydedildi.`);
    });
    let dragDepth = 0;
    const transferTypes = (event) => Array.from(event.dataTransfer?.types || []);
    const acceptsTransfer = (event) => transferTypes(event).includes(transferType) || transferTypes(event).includes(internalTransferType);
    const clearDrop = () => { dragDepth = 0; workspace.classList.remove('is-drop-target'); rows.querySelectorAll('.is-drop-before, .is-drop-after, .is-drop-into').forEach((row) => row.classList.remove('is-drop-before', 'is-drop-after', 'is-drop-into')); };
    const dropPlacement = (event, detail = {}, moving = null, mark = false) => {
        const target = event.target instanceof Element ? event.target : null;
        const row = target?.closest('tr[data-programme-item]');
        const gap = row ? null : target?.closest('tr[data-programme-transition-row]');
        const empty = row || gap ? null : target?.closest('tr[data-programme-parent]');
        const ref = findRef(row?.dataset.programmeItem || gap?.dataset.programmeTransitionRow || empty?.dataset.programmeParent);
        if (mark) clearDrop();
        if (!ref) return { parentId: null, index: programme.items.length };
        const topKind = detail.kind === 'programme' || moving?.item.isBlock;
        if (ref.item.isBlock && !topKind) {
            if (mark) (row || empty)?.classList.add('is-drop-into');
            return { parentId: ref.item.id, index: ref.item.contents.length };
        }
        if (topKind) {
            let rootItem = ref.item; for (let parent = ref.parent; parent; parent = findRef(parent.id)?.parent) rootItem = parent;
            const rect = (row || gap || empty)?.getBoundingClientRect();
            const before = row && rect && event.clientY < rect.top + rect.height / 2;
            if (mark) row?.classList.add(before ? 'is-drop-before' : 'is-drop-after');
            return { parentId: null, index: programme.items.indexOf(rootItem) + (before ? 0 : 1) };
        }
        const rect = (row || gap || empty)?.getBoundingClientRect();
        const before = row && rect && event.clientY < rect.top + rect.height / 2;
        if (mark) row?.classList.add(before ? 'is-drop-before' : 'is-drop-after');
        return { parentId: ref.parent?.id || null, index: ref.index + (before ? 0 : 1) };
    };
    rows.addEventListener('dragstart', (event) => {
        const row = event.target.closest('tr[data-programme-item]'), ref = findRef(row?.dataset.programmeItem);
        if (!ref || isLocked(ref) || !event.dataTransfer) { event.preventDefault(); return; }
        event.dataTransfer.effectAllowed = 'copyMove';
        event.dataTransfer.setData(internalTransferType, JSON.stringify({ id: ref.item.id, parentId: ref.parent?.id || null }));
        event.dataTransfer.setData(transferType, JSON.stringify({ kind: ref.item.isBlock ? 'programme' : 'items', programmeName: ref.item.title, items: leavesOf(ref.item).map(copy), playlistName: programme.name }));
        event.dataTransfer.setData('text/plain', ref.item.title);
    });
    workspace.addEventListener('dragenter', (event) => { if (!acceptsTransfer(event)) return; event.preventDefault(); dragDepth += 1; workspace.classList.add('is-drop-target'); });
    workspace.addEventListener('dragover', (event) => {
        if (!acceptsTransfer(event)) return; event.preventDefault();
        const internal = transferTypes(event).includes(internalTransferType);
        event.dataTransfer.dropEffect = internal ? 'move' : 'copy';
        let moving = null; try { if (internal) moving = findRef(JSON.parse(event.dataTransfer.getData(internalTransferType)).id); } catch { /* The drag payload may be protected until drop. */ }
        dropPlacement(event, {}, moving, true); workspace.classList.add('is-drop-target');
    });
    workspace.addEventListener('dragleave', (event) => { if (!acceptsTransfer(event)) return; dragDepth = Math.max(0, dragDepth - 1); if (!dragDepth) clearDrop(); });
    workspace.addEventListener('drop', (event) => {
        if (!acceptsTransfer(event)) return; event.preventDefault(); event.stopPropagation();
        try {
            if (transferTypes(event).includes(internalTransferType)) {
                const payload = JSON.parse(event.dataTransfer.getData(internalTransferType)), ref = findRef(payload.id), placement = dropPlacement(event, {}, ref);
                moveItem(payload.id, placement.index, placement.parentId);
            } else {
                const detail = JSON.parse(event.dataTransfer.getData(transferType));
                addTransferredItems({ ...detail, mode: 'insert' }, dropPlacement(event, detail));
            }
        } catch { announce('Bu aktarım okunamadı.'); }
        clearDrop();
    });
    document.addEventListener('dragend', clearDrop);
    rows.addEventListener('click', (event) => {
        if (!(event.target instanceof Element) || event.target.closest('button, a, input, select, textarea')) return;
        event.target.closest('tr[data-programme-item]')?.querySelector('[data-select-programme-leaf], [data-select-programme-item]')?.click();
    });

    const tabs = Array.from(document.querySelectorAll('[data-programme-bank-tab]'));
    const selectBank = (selected, focus = false) => {
        for (const tab of tabs) {
            const active = tab === selected;
            tab.setAttribute('aria-selected', String(active));
            tab.tabIndex = active ? 0 : -1;
            const panel = document.getElementById(tab.getAttribute('aria-controls'));
            if (panel) panel.hidden = !active;
        }
        if (focus) selected?.focus();
    };

    document.addEventListener('click', (event) => {
        if (!(event.target instanceof Element)) return;
        const target = event.target.closest('button');
        if (!target || target.disabled) return;

        if (target.matches('[data-programme-block-toggle]')) {
            const ref = findRef(target.dataset.programmeBlockToggle); if (!ref?.item.isBlock) return;
            selectedId = ref.item.id;
            if (expanded.has(ref.item.id)) expanded.delete(ref.item.id); else expanded.add(ref.item.id);
            render(); announce(`${ref.item.title} içeriği ${expanded.has(ref.item.id) ? 'açıldı' : 'kapatıldı'}.`);
        } else if (target.matches('[data-programme-queue-action]')) {
            const action = target.dataset.programmeQueueAction;
            if (['top', 'bottom', 'up', 'down'].includes(action)) runMove(action);
            else if (action === 'play') startGui(selectedItem());
            else if (action === 'previous' || action === 'next') stepGui(action === 'next' ? 1 : -1);
            else if (action === 'pause') { if (guiPlayback.itemId) guiPlayback.state = 'paused'; render(); announce('Duraklatıldı.'); }
            else if (action === 'stop') { guiPlayback = { itemId: null, state: 'idle' }; render(); announce('Durduruldu.'); }
            else if (['insert', 'replace', 'input'].includes(action)) requestLibrary(action === 'input' ? 'append' : action);
            else if (action === 'clear') {
                const block = activeBlock(); if (block && isLocked(findRef(block.id))) { announce('Kilitli blok temizlenemez.'); return; }
                const preserveLocked = (item) => {
                    if (item.readiness === 'locked') return true;
                    if (item.isBlock) { item.contents = item.contents.filter(preserveLocked); return Boolean(item.contents.length); }
                    return false;
                };
                if (block) { block.contents = block.contents.filter(preserveLocked); selectedId = block.id; }
                else { programme.items = programme.items.filter(preserveLocked); selectedId = programme.items[0]?.id || null; }
                changed(); render(); announce(`${block ? block.title + ' içeriği' : 'Yayın sırası'} temizlendi; kilitli içerikler korundu.`);
            }
        } else if (target.matches('[data-programme-row-action]')) {
            const ref = findRef(target.dataset.programmeRowItem); if (!ref) return;
            selectedId = ref.item.id;
            const action = target.dataset.programmeRowAction;
            if (action === 'play') startGui(ref.item);
            else if (action === 'waveform') { selectItem(ref.item.id, { openWaveform: !ref.item.isBlock }); announce(`${ref.item.title} ${ref.item.isBlock ? 'bloğu seçildi' : 'cue ve geçişleri düzenleniyor'}.`); }
            else if (action === 'properties') { render(); openEditor('programme-item-editor', ref.item); }
            else if (action === 'transition') { selectItem(ref.item.id, { openWaveform: true }); }
            else if (action === 'remove') removeItem(ref.item.id);
            else if (action === 'up' || action === 'down') runMove(action);
        } else if (target.matches('[data-programme-transition]')) {
            const ref = findRef(target.dataset.programmeTransition); if (!ref || ref.item.isBlock) return;
            selectItem(ref.item.id, { openWaveform: true });
        } else if (target.matches('[data-select-programme-leaf]')) {
            if (selectItem(target.dataset.selectProgrammeLeaf, { openWaveform: true })) announce(`${selectedItem().title} cue ve geçişleri düzenleniyor.`);
        } else if (target.matches('[data-select-programme-item]')) {
            if (selectItem(target.dataset.selectProgrammeItem)) announce(`${selectedItem().title} eventi: içerikler gösteriliyor.`);
        } else if (target.matches('[data-add-programme-item]')) {
            const type = target.dataset.addProgrammeItem; if (!Object.hasOwn(labels, type)) return;
            const item = makeBlock(type, target.dataset.itemTitle || eventTitles[type]);
            programme.items.push(item); selectedId = item.id; if (item.isBlock) expanded.add(item.id);
            changed(); render(); announce(`${item.title} eklendi.`);
        } else if (target.matches('[data-programme-action]')) {
            const ref = selectedRef(); if (!ref) return;
            if (isLocked(ref)) { announce('Bu içerik kilitli. Özelliklerden kilidi kaldır.'); return; }
            const action = target.dataset.programmeAction;
            if (action === 'remove') removeItem(ref.item.id);
            else if (action === 'up' || action === 'down') runMove(action);
        } else if (target.matches('[data-open-programme-editor]')) {
            openEditor('programme-editor', { name: programme.name, date: programme.date, notes: programme.notes });
        } else if (target.matches('[data-open-programme-properties]')) {
            const item = selectedItem();
            if (item) openEditor('programme-item-editor', item);
        } else if (target.matches('[data-programme-save]')) {
            const date = new Date().toISOString();
            try {
                localStorage.setItem(storageKey, JSON.stringify({ programme, savedAt: date, selectedId, expandedBlocks: [...expanded] }));
                savedRevision = programme.revision;
                savedAt = date;
                render();
                announce('Kaydedildi.');
            } catch {
                announce('Kaydedilemedi.');
            }
        } else if (target.matches('[data-programme-review], [data-programme-send-review]')) {
            if (target.matches('[data-programme-send-review]')) {
                programme.stage = 'review';
                programme.revision += 1;
                render();
            }
            renderReview();
            openEditor('programme-review-dialog', {});
        } else if (target.matches('[data-programme-approve]')) {
            if (programme.stage !== 'review' || !programme.items.length) return;
            programme.stage = 'local-approved';
            programme.revision += 1;
            render();
            closeDialog(target.closest('dialog'));
            announce('Onaylandı.');
        } else if (target.matches('[data-programme-timing-edit]')) {
            openEditor('programme-timing-editor', programme.timing);
            updateTimingWait();
        } else if (target.matches('[data-close-programme-dialog]')) {
            closeDialog(target.closest('dialog'));
        } else if (target.matches('[data-programme-bank-tab]')) {
            selectBank(target);
        }
    });

    const renderReview = () => {
        const container = document.querySelector('[data-programme-review-content]');
        if (!container) return;
        const summary = element('p', 'programme-review-summary', `${programme.name} · Revizyon ${programme.revision} · ${programme.items.length} öğe`);
        const storage = element('p', 'programme-review-summary', savedRevision === programme.revision ? 'Revizyon kaydedildi.' : 'Bu revizyonda kaydedilmemiş değişiklikler var.');
        const list = element('ul', 'programme-review-list');
        const playback = leafRefs().map(({ item }) => item);
        const unready = playback.filter((item) => item.readiness !== 'ready');
        const unknown = playback.filter((item) => !validDuration(item.duration));
        list.append(element('li', '', `Hazır olmayan: ${unready.length} öğe`), element('li', '', `Süresi eksik: ${unknown.length} öğe`));
        for (const item of playback) list.append(element('li', 'programme-review-item', `${item.title} · ${item.version} · ${readinessLabels[item.readiness]}`));
        container.replaceChildren(summary, storage, list);
        const approve = document.querySelector('[data-programme-approve]');
        if (approve) approve.disabled = programme.stage !== 'review' || !programme.items.length;
    };
    const updateTimingWait = () => {
        const form = document.querySelector('[data-programme-timing-form]');
        const input = field(form, 'maxWait');
        if (input) input.disabled = readField(form, 'policy') !== 'finish';
    };
    document.addEventListener('change', (event) => {
        if (event.target instanceof Element && event.target.matches('[data-programme-timing-form] [name="policy"]')) updateTimingWait();
    });

    const transitionMode = workspace.querySelector('[data-programme-transition-mode]');
    const transitionOverlap = workspace.querySelector('[data-programme-transition-overlap]');
    const syncTransitionControls = ({ mode, overlap } = {}) => {
        if (!transitionMode || !transitionOverlap) return;
        if (['end', 'crossfade', 'cue', 'manual'].includes(mode)) transitionMode.value = mode;
        if (Number.isFinite(Number(overlap))) transitionOverlap.value = String(Math.max(0, Math.min(30, Number(overlap))));
        transitionOverlap.disabled = ['end', 'manual'].includes(transitionMode.value);
    };
    const changeTransition = () => {
        if (!transitionMode || !transitionOverlap) return;
        if (['crossfade', 'cue'].includes(transitionMode.value) && !transitionOverlap.checkValidity()) { transitionOverlap.reportValidity(); return; }
        const settings = { mode: transitionMode.value, overlap: Number(transitionOverlap.value) };
        syncTransitionControls(settings);
        window.RadioSAutomation?.setTransition?.(settings);
    };
    transitionMode?.addEventListener('change', changeTransition);
    transitionOverlap?.addEventListener('change', changeTransition);
    document.addEventListener('radios:automation-transition-change', (event) => syncTransitionControls(event.detail));
    workspace.querySelector('[data-programme-transition-settings]')?.addEventListener('click', () => {
        document.getElementById('profile-automation-tab')?.click();
        document.getElementById('automation-timing-tab')?.click();
        document.getElementById('automation-transition')?.focus();
    });
    syncTransitionControls();

    document.addEventListener('keydown', (event) => {
        if (!(event.target instanceof Element)) return;
        const tab = event.target.closest('[data-programme-bank-tab]');
        if (!tab || !tabs.includes(tab)) return;
        const index = tabs.indexOf(tab);
        let next;
        if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
        if (event.key === 'ArrowLeft') next = (index - 1 + tabs.length) % tabs.length;
        if (event.key === 'Home') next = 0;
        if (event.key === 'End') next = tabs.length - 1;
        if (next === undefined) return;
        event.preventDefault();
        selectBank(tabs[next], true);
    });

    document.addEventListener('input', (event) => {
        if (!(event.target instanceof Element)) return;
        if (!event.target.closest('[data-programme-form], [data-programme-item-form], [data-programme-timing-form]')) return;
        if (typeof event.target.setCustomValidity === 'function') event.target.setCustomValidity('');
        if (event.target.closest('[data-programme-item-form]')) field(event.target.closest('form'), 'readiness')?.setCustomValidity('');
    });

    document.addEventListener('submit', (event) => {
        const form = event.target;
        if (!(form instanceof HTMLFormElement)) return;
        const isProgramme = form.matches('[data-programme-form]');
        const isItem = form.matches('[data-programme-item-form]');
        const isTiming = form.matches('[data-programme-timing-form]');
        if (!isProgramme && !isItem && !isTiming) return;
        event.preventDefault();
        if (isTiming) {
            if (!form.reportValidity()) return;
            const start = readField(form, 'start');
            const target = readField(form, 'target');
            if (!validTime(start) || !validTime(target)) return;
            const policy = readField(form, 'policy');
            if (!['cut', 'finish'].includes(policy)) return;
            programme.timing = {
                title: readField(form, 'title') || 'Kesin saatli olay', start, target, policy,
                maxWait: policy === 'finish' ? Math.max(0, Math.min(600, Number(readField(form, 'maxWait')) || 0)) : programme.timing.maxWait,
            };
            changed();
            render();
            announce('Kesin saat güncellendi.');
            closeDialog(form.closest('dialog'));
            return;
        }

        const titleField = field(form, isProgramme ? 'name' : 'title');
        const title = readField(form, isProgramme ? 'name' : 'title');
        if (!title && titleField?.setCustomValidity) titleField.setCustomValidity('Bir başlık yazın.');
        if (!form.reportValidity() || !title) return;

        if (isProgramme) {
            programme.name = title;
            programme.date = readField(form, 'date');
            programme.notes = readField(form, 'notes');
            changed();
            render();
            announce('Program bilgileri güncellendi.');
        } else {
            const item = form.dataset.programmeEditingId ? findRef(form.dataset.programmeEditingId)?.item : selectedItem();
            if (!item) return;
            const type = readField(form, 'type') || item.type;
            if (!Object.hasOwn(labels, type)) return;
            if (type === 'programme' && !item.isBlock) { field(form, 'type')?.setCustomValidity('Program için ayrı bir program bloğu ekle.'); form.reportValidity(); return; }
            const readiness = readField(form, 'readiness') || item.readiness;
            if (!Object.hasOwn(readinessLabels, readiness)) return;
            const primaryState = readField(form, 'primaryState') || item.primaryState;
            const backupState = readField(form, 'backupState') || item.backupState;
            if (!['unknown', 'present', 'missing'].includes(primaryState) || !['unknown', 'present', 'missing'].includes(backupState)) return;
            if (readiness === 'ready' && (primaryState !== 'present' || backupState !== 'present')) {
                field(form, 'readiness')?.setCustomValidity('Hazır için ana ve yedek dosya Mevcut olmalı.');
                form.reportValidity();
                return;
            }
            Object.assign(item, {
                title,
                type,
                artist: readField(form, 'artist'),
                album: readField(form, 'album'),
                time: readField(form, 'time'),
                duration: item.isBlock ? item.duration : readField(form, 'duration'),
                notes: readField(form, 'notes'),
                version: readField(form, 'version') || item.version,
                readiness, primaryState, backupState,
            });
            if (item.markers) item.markers = normalizeMarkers(item.markers, item);
            selectedId = item.id;
            changed();
            render();
            announce(`${item.title} özellikleri güncellendi.`);
        }
        closeDialog(form.closest('dialog'));
    });

    render();
    announce(restored ? 'Kayıtlı program yüklendi.' : '');
    if (tabs.length) selectBank(tabs.find((tab) => tab.getAttribute('aria-selected') === 'true') || tabs[0]);
})();
