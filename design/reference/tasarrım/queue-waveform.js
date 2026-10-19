(() => {
    'use strict';
    const dialog = document.getElementById('queue-waveform-dialog');
    const form = dialog?.querySelector('[data-queue-waveform-form]');
    if (!dialog || !form) return;
    const q = (selector) => dialog.querySelector(selector);
    const fields = ['cueIn', 'intro', 'out', 'fadeStart', 'fadeEnd', 'autoNext', 'overlap'];
    const labels = { cueIn: 'Cue giriş', intro: 'Vokal / intro', out: 'Cue çıkış', fadeStart: 'Fade başlangıcı', fadeEnd: 'Fade bitişi', autoNext: 'Otomatik geçiş noktası', overlap: 'Bindirme' };
    const tags = { cueIn: 'IN', intro: 'VO', out: 'OUT', fadeStart: 'F1', fadeEnd: 'F2', autoNext: 'AUTO' };
    const copy = (value) => JSON.parse(JSON.stringify(value));
    const field = (name) => form.elements.namedItem(name);
    const text = (value, fallback = '') => typeof value === 'string' && value.trim() ? value.trim() : fallback;
    const number = (value) => value === null || value === undefined || value === '' ? null : Number.isFinite(Number(value)) && Number(value) >= 0 ? Math.round(Number(value) * 10) / 10 : null;
    const durationOf = (item) => {
        if (Number.isFinite(item?.durationSeconds) && item.durationSeconds > 0) return item.durationSeconds;
        if (Number.isFinite(item?.duration) && item.duration > 0) return item.duration;
        const match = typeof item?.duration === 'string' && item.duration.match(/^(\d{1,3}):([0-5]\d)$/);
        return match && Number(match[1]) * 60 + Number(match[2]) > 0 ? Number(match[1]) * 60 + Number(match[2]) : null;
    };
    const defaults = (duration) => ({ cueIn: 0, intro: null, out: duration, fadeStart: null, fadeEnd: null, autoNext: null, overlap: 0, automatic: false });
    const markersOf = (item) => {
        const source = item?.markers && typeof item.markers === 'object' ? item.markers : {};
        const markers = defaults(durationOf(item));
        fields.forEach((key) => {
            if (Object.hasOwn(source, key)) markers[key] = number(source[key]);
        });
        markers.cueIn ??= 0;
        markers.overlap ??= 0;
        markers.automatic = source.automatic === true;
        return markers;
    };
    const formatTime = (value) => {
        if (!Number.isFinite(value) || value < 0) return '—:—';
        const tenths = Math.round(value * 10);
        const minutes = Math.floor(tenths / 600);
        const seconds = Math.floor(tenths % 600 / 10);
        return `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}${tenths % 10 ? `.${tenths % 10}` : ''}`;
    };
    const sequence = () => {
        try {
            const snapshot = window.RadioSProgramme?.getSnapshot?.();
            if (Array.isArray(snapshot?.playbackItems)) return snapshot.playbackItems;
            const editable = window.RadioSProgramme?.getEditableSequence?.();
            if (Array.isArray(editable)) return editable;
            if (Array.isArray(editable?.items)) return editable.items;
            if (Array.isArray(snapshot?.programme?.items)) return snapshot.programme.items.flatMap((item) => Array.isArray(item.contents) ? item.contents : [item]);
        } catch { /* An unavailable model never becomes a real playback result. */ }
        return null;
    };
    const lockedItem = (item) => {
        if (item.readiness === 'locked') return true;
        try {
            const visit = (items, locked = false) => {
                for (const entry of items || []) {
                    const currentLocked = locked || entry.readiness === 'locked';
                    if (entry.id === item.id) return currentLocked;
                    if (Array.isArray(entry.contents)) {
                        const result = visit(entry.contents, currentLocked);
                        if (result !== null) return result;
                    }
                }
                return null;
            };
            return visit(window.RadioSProgramme?.getSnapshot?.()?.programme?.items) === true;
        } catch { return false; }
    };
    let state = null;
    let origin = null;
    let pointer = null;
    let saving = false;
    const closeEditor = () => {
        if (state) state.playing = false;
        dialog.close();
    };
    const capture = () => ({ draft: copy(state.draft), cursor: state.cursor });
    const dirty = () => Boolean(state && JSON.stringify(state.draft) !== JSON.stringify(state.base));
    const pushHistory = (before) => {
        if (!state || JSON.stringify(before.draft) === JSON.stringify(state.draft)) return;
        state.undo.push(before);
        if (state.undo.length > 100) state.undo.shift();
        state.redo = [];
    };
    const finishInput = () => {
        if (!state?.editBefore) return;
        pushHistory(state.editBefore);
        state.editBefore = null;
    };
    const setStatus = (message = '', warning = false) => {
        if (!state) return;
        state.message = message;
        state.warning = warning;
        q('[data-qwf-status]').textContent = message || '';
        q('[data-qwf-status]').classList.toggle('has-warning', warning);
    };
    const fitView = () => { state.view = { start: 0, end: state.duration || 0 }; };
    const overlapLimit = () => {
        const limits = [];
        if (state.duration !== null) limits.push(state.duration);
        if (Number.isFinite(state.draft.out) && Number.isFinite(state.draft.cueIn) && state.draft.out >= state.draft.cueIn) limits.push(state.draft.out - state.draft.cueIn);
        const nextDuration = durationOf(state.next);
        if (nextDuration !== null) limits.push(nextDuration);
        return limits.length ? Math.min(...limits) : null;
    };
    const syncInputs = () => {
        fields.forEach((key) => { field(key).value = state.draft[key] === null ? '' : String(state.draft[key]); });
        field('automatic').checked = state.draft.automatic;
    };
    const render = (inputs = false) => {
        if (!state) return;
        if (inputs) syncInputs();
        q('[data-qwf-title]').textContent = text(state.item.title, 'Adsız parça');
        const types = { music: 'Müzik', announcement: 'Anons', advert: 'Reklam' };
        q('[data-qwf-meta]').textContent = [state.item.artist, state.item.album, types[state.item.type] || 'Parça'].filter((entry) => typeof entry === 'string' && entry.trim()).join(' · ');
        q('[data-qwf-duration]').textContent = state.duration === null ? 'Bilinmiyor' : formatTime(state.duration);
        q('[data-qwf-next]').textContent = state.next ? text(state.next.title, 'Adsız parça') : 'Sonraki parça yok';
        const readOnly = state.missing || state.locked;
        q('[data-qwf-fields]').disabled = readOnly;
        fields.forEach((key) => {
            const max = key === 'overlap' ? overlapLimit() : state.duration;
            if (max !== null) field(key).max = String(max);
            else field(key).removeAttribute('max');
        });
        field('out').required = state.duration !== null;
        field('overlap').disabled = readOnly || !state.next;
        field('automatic').disabled = readOnly || !state.next;
        const timed = state.duration !== null && !readOnly;
        dialog.querySelectorAll('[data-qwf-set],[data-qwf-zoom]').forEach((button) => { button.disabled = !timed; });
        q('[data-qwf-undo]').disabled = readOnly || !state.undo.length;
        q('[data-qwf-redo]').disabled = readOnly || !state.redo.length;
        q('[data-qwf-reset]').disabled = readOnly;
        q('[data-qwf-save]').disabled = readOnly;
        const play = q('[data-qwf-play]');
        play.disabled = readOnly;
        play.setAttribute('aria-pressed', String(state.playing));
        play.textContent = state.playing ? 'Ⅱ Duraklat' : '▶ Oynat';
        q('[data-qwf-position]').value = timed ? formatTime(state.cursor) : '—:—';
        const timeline = q('[data-qwf-timeline]');
        timeline.setAttribute('aria-disabled', String(!timed));
        const layer = q('[data-qwf-marker-layer]');
        layer.replaceChildren();
        const axis = q('[data-qwf-time-axis]');
        axis.replaceChildren();
        const cursor = q('[data-qwf-cursor]');
        cursor.hidden = !timed || state.cursor < state.view.start || state.cursor > state.view.end;
        if (timed) {
            const span = state.view.end - state.view.start;
            cursor.style.left = `${(state.cursor - state.view.start) / span * 100}%`;
            Object.keys(tags).forEach((key, index) => {
                const value = state.draft[key];
                if (!Number.isFinite(value) || value < state.view.start || value > state.view.end) return;
                const marker = document.createElement('button');
                marker.type = 'button';
                marker.className = 'qwf-marker';
                marker.dataset.qwfMarker = key;
                marker.textContent = tags[key];
                marker.title = `${labels[key]} · ${formatTime(value)}`;
                marker.setAttribute('aria-label', marker.title);
                marker.style.setProperty('--qwf-position', `${(value - state.view.start) / span * 100}%`);
                marker.style.setProperty('--qwf-lane', `${5 + index * 26}px`);
                layer.append(marker);
            });
            for (let index = 0; index <= 4; index += 1) {
                const tick = document.createElement('span');
                tick.textContent = formatTime(state.view.start + span * index / 4);
                axis.append(tick);
            }
        } else {
            const waiting = document.createElement('span');
            waiting.textContent = state.missing ? 'Parça akışta yok' : '';
            axis.append(waiting);
        }
        setStatus(state.message, state.warning);
    };
    const refreshModel = () => {
        if (!state || saving || !dialog.open) return;
        const items = sequence();
        if (items === null) return;
        const index = items.findIndex((item) => item.id === state.item.id);
        if (index < 0) {
            state.missing = true;
            state.playing = false;
            setStatus('Parça akıştan kaldırıldı; işaretler kaydedilemez.', true);
            render();
            return;
        }
        const next = items[index + 1] || null;
        const nextChanged = (next?.id || null) !== (state.next?.id || null);
        state.item = copy(items[index]);
        state.missing = false;
        state.locked = lockedItem(state.item);
        if (state.locked) {
            state.playing = false;
            setStatus('Parça veya içinde bulunduğu blok kilitli. İşaretler düzenlenemez.', true);
        }
        const duration = durationOf(state.item);
        if (duration !== state.duration) {
            state.duration = duration;
            state.cursor = duration === null ? 0 : Math.min(state.cursor, duration);
            fitView();
            setStatus('Parça süresi değişti. İşaretlerin yeni süreye uyduğunu kontrol et.', true);
        }
        state.next = next ? copy(next) : null;
        if (nextChanged) {
            finishInput();
            state.draft.overlap = 0;
            state.draft.automatic = false;
            state.playing = false;
            [...state.undo, ...state.redo].forEach((entry) => {
                entry.draft.overlap = 0;
                entry.draft.automatic = false;
            });
            setStatus('Sonraki parça değişti. Bindirme sıfırlandı; otomatik geçişi yeniden kontrol et.', true);
        }
        render(nextChanged);
    };
    const open = (detail) => {
        if (!detail?.item?.id) return;
        const items = sequence();
        const index = items?.findIndex((item) => item.id === detail.item.id) ?? -1;
        const item = index >= 0 ? items[index] : detail.item;
        const next = index >= 0 ? items[index + 1] || null : detail.nextItem || null;
        origin = document.activeElement;
        state = { item: copy(item), next: next ? copy(next) : null, blockId: detail.blockId || item.parentId || null, duration: durationOf(item), base: markersOf(item), draft: markersOf(item), cursor: 0, view: null, playing: false, missing: items !== null && index < 0, locked: lockedItem(item), message: '', warning: false, undo: [], redo: [], editBefore: null, saved: false };
        state.cursor = state.draft.cueIn;
        if (!state.next) { state.draft.overlap = 0; state.draft.automatic = false; }
        if (item.markers?.nextId && item.markers.nextId !== state.next?.id) {
            state.draft.overlap = 0;
            state.draft.automatic = false;
            setStatus('Sonraki parça değişti; bindirme kontrol edilmeli.', true);
        }
        fitView();
        [...form.elements].forEach((input) => { if ('setCustomValidity' in input) input.setCustomValidity(''); });
        if (state.missing) setStatus('Parça akışta yok; işaretler kaydedilemez.', true);
        else if (state.locked) setStatus('Parça veya içinde bulunduğu blok kilitli. İşaretler düzenlenemez.', true);
        render(true);
        if (!dialog.open) dialog.showModal();
        q('[data-qwf-close]').focus();
    };
    const editMarker = (key, value) => {
        if (!state || state.missing || state.locked || !fields.includes(key)) return;
        finishInput();
        const before = capture();
        state.draft[key] = Math.round(value * 10) / 10;
        state.message = '';
        state.warning = false;
        pushHistory(before);
        render(true);
    };
    const timelineValue = (event) => {
        const rect = q('[data-qwf-timeline]').getBoundingClientRect();
        const ratio = Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width));
        return Math.round((state.view.start + ratio * (state.view.end - state.view.start)) * 10) / 10;
    };
    const timeline = q('[data-qwf-timeline]');
    timeline.addEventListener('pointerdown', (event) => {
        if (!state || state.duration === null || state.missing || state.locked || event.button !== 0 || pointer) return;
        event.preventDefault();
        finishInput();
        const marker = event.target.closest('[data-qwf-marker]');
        pointer = { id: event.pointerId, key: marker?.dataset.qwfMarker || null, before: capture(), startX: event.clientX };
        timeline.focus({ preventScroll: true });
        timeline.setPointerCapture(event.pointerId);
        state.cursor = pointer.key ? state.draft[pointer.key] : timelineValue(event);
        render(Boolean(pointer.key));
    });
    timeline.addEventListener('pointermove', (event) => {
        if (!pointer || event.pointerId !== pointer.id) return;
        event.preventDefault();
        if (pointer.key) {
            const rect = timeline.getBoundingClientRect();
            const offset = (event.clientX - pointer.startX) / rect.width * (state.view.end - state.view.start);
            state.cursor = Math.round(Math.max(state.view.start, Math.min(state.view.end, pointer.before.draft[pointer.key] + offset)) * 10) / 10;
        } else state.cursor = timelineValue(event);
        if (pointer.key) state.draft[pointer.key] = state.cursor;
        render(Boolean(pointer.key));
    });
    const finishPointer = (event) => {
        if (!pointer || event.pointerId !== pointer.id) return;
        const drag = pointer;
        pointer = null;
        pushHistory(drag.before);
        if (timeline.hasPointerCapture(event.pointerId)) timeline.releasePointerCapture(event.pointerId);
        render();
    };
    ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((name) => timeline.addEventListener(name, finishPointer));
    timeline.addEventListener('keydown', (event) => {
        if (!state || state.duration === null || state.missing || state.locked || event.ctrlKey || event.metaKey || event.altKey) return;
        const marker = event.target.closest('[data-qwf-marker]');
        const key = marker?.dataset.qwfMarker;
        let value = key ? state.draft[key] : state.cursor;
        const step = event.shiftKey ? 1 : .1;
        if (event.key === 'ArrowLeft' || event.key === 'ArrowDown') value -= step;
        else if (event.key === 'ArrowRight' || event.key === 'ArrowUp') value += step;
        else if (event.key === 'Home') value = 0;
        else if (event.key === 'End') value = state.duration;
        else return;
        event.preventDefault();
        value = Math.min(state.duration, Math.max(0, value));
        state.cursor = Math.round(value * 10) / 10;
        if (key) {
            editMarker(key, state.cursor);
            q(`[data-qwf-marker="${key}"]`)?.focus({ preventScroll: true });
        } else render();
    });
    form.addEventListener('focusin', (event) => {
        if (state && fields.includes(event.target.name) && !state.editBefore) state.editBefore = capture();
    });
    form.addEventListener('input', (event) => {
        if (!state || !fields.includes(event.target.name)) return;
        if (!state.editBefore) state.editBefore = capture();
        state.draft[event.target.name] = event.target.value === '' ? null : Number(event.target.value);
        state.message = '';
        state.warning = false;
        [...form.elements].forEach((input) => { if ('setCustomValidity' in input) input.setCustomValidity(''); });
        render();
    });
    form.addEventListener('change', (event) => {
        if (!state) return;
        if (fields.includes(event.target.name)) { finishInput(); render(); }
        if (event.target.name === 'automatic') {
            const before = capture();
            state.draft.automatic = event.target.checked;
            pushHistory(before);
            render();
        }
    });
    form.addEventListener('focusout', (event) => { if (fields.includes(event.target.name)) { finishInput(); render(); } });
    dialog.addEventListener('click', (event) => {
        const button = event.target.closest('button');
        if (!button || button.disabled) return;
        if (button.hasAttribute('data-qwf-close')) { closeEditor(); return; }
        if (!state) return;
        const key = button.dataset.qwfSet;
        if (key) editMarker(key, state.cursor);
        if (button.hasAttribute('data-qwf-reset')) {
            finishInput();
            const before = capture();
            state.draft = defaults(state.duration);
            pushHistory(before);
            render(true);
        }
        if (button.hasAttribute('data-qwf-undo') || button.hasAttribute('data-qwf-redo')) {
            finishInput();
            const undo = button.hasAttribute('data-qwf-undo');
            const from = undo ? state.undo : state.redo;
            const to = undo ? state.redo : state.undo;
            if (from.length) {
                to.push(capture());
                const restored = from.pop();
                state.draft = restored.draft;
                state.cursor = restored.cursor;
                render(true);
            }
        }
        if (button.hasAttribute('data-qwf-play')) {
            state.playing = !state.playing;
            setStatus(state.playing ? 'Oynatılıyor.' : 'Duraklatıldı.');
            render();
        }
        const zoom = button.dataset.qwfZoom;
        if (zoom && state.duration !== null) {
            if (zoom === 'fit') fitView();
            else {
                const size = Math.min(state.duration, Math.max(Math.min(1, state.duration), (state.view.end - state.view.start) * (zoom === 'in' ? .5 : 2)));
                const start = Math.min(state.duration - size, Math.max(0, state.cursor - size / 2));
                state.view = { start, end: start + size };
            }
            render();
        }
    });
    const validate = () => {
        [...form.elements].forEach((input) => { if ('setCustomValidity' in input) input.setCustomValidity(''); });
        const draft = state.draft;
        const error = (key, message) => field(key).setCustomValidity(message);
        fields.forEach((key) => { if (draft[key] !== null && (!Number.isFinite(draft[key]) || draft[key] < 0)) error(key, 'Sıfır veya daha büyük bir saniye değeri gir.'); });
        if (draft.out !== null && draft.cueIn !== null && draft.out < draft.cueIn) error('out', 'Cue çıkış, cue girişten önce olamaz.');
        ['intro', 'fadeStart', 'fadeEnd', 'autoNext'].forEach((key) => {
            if (draft[key] === null) return;
            if (draft.cueIn !== null && draft[key] < draft.cueIn) error(key, 'İşaret, cue girişten önce olamaz.');
            if (draft.out !== null && draft[key] > draft.out) error(key, 'İşaret, cue çıkıştan sonra olamaz.');
        });
        if ((draft.fadeStart === null) !== (draft.fadeEnd === null)) error(draft.fadeStart === null ? 'fadeStart' : 'fadeEnd', 'Fade başlangıcı ve bitişi birlikte girilmeli.');
        if (draft.fadeStart !== null && draft.fadeEnd !== null && draft.fadeEnd < draft.fadeStart) error('fadeEnd', 'Fade bitişi başlangıçtan önce olamaz.');
        if (draft.automatic && draft.autoNext === null) error('autoNext', 'Otomatik geçiş noktası gerekli.');
        return form.reportValidity();
    };
    form.addEventListener('submit', (event) => {
        event.preventDefault();
        if (!state) return;
        finishInput();
        refreshModel();
        if (state.missing || state.locked || !validate()) return;
        const markers = copy(state.draft);
        markers.overlap ??= 0;
        if (!state.next) { markers.overlap = 0; markers.automatic = false; }
        state.playing = false;
        state.saved = true;
        state.base = copy(markers);
        state.draft = copy(markers);
        saving = true;
        try {
            document.dispatchEvent(new CustomEvent('radios:programme-markers-saved', { detail: { itemId: state.item.id, markers, nextId: state.next?.id || null } }));
        } finally { saving = false; }
        closeEditor();
    });
    dialog.addEventListener('cancel', () => { if (state) state.playing = false; });
    dialog.addEventListener('close', () => {
        if (state) state.playing = false;
        pointer = null;
        if (origin?.isConnected && !origin.closest('[hidden]')) origin.focus({ preventScroll: true });
        origin = null;
    });
    document.addEventListener('radios:programme-waveform-request', (event) => open(event.detail));
    document.addEventListener('radios:programme-change', refreshModel);
    document.addEventListener('radios:profile-change', (event) => { if (event.detail?.profile !== 'broadcast' && dialog.open) closeEditor(); });
    window.RadioSQueueWaveform = Object.freeze({ snapshot: () => state ? copy({ open: dialog.open, itemId: state.item.id, blockId: state.blockId, nextId: state.next?.id || null, duration: state.duration, draft: state.draft, base: state.base, cursor: state.cursor, view: state.view, playing: state.playing, missing: state.missing, locked: state.locked, dirty: dirty(), saved: state.saved, undoCount: state.undo.length, redoCount: state.redo.length }) : null });
})();
