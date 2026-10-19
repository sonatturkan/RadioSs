(() => {
    'use strict';

    const rack = document.querySelector('[data-dj-effects-rack]');
    const template = document.getElementById('dj-effect-template');
    if (!rack || !(template instanceof HTMLTemplateElement) || window.RadioSDJEffects) return;

    const storageKey = 'radios.dj-effects.v1';
    const archiveKey = 'radios.dj-effects.previous-rack.v1';
    const layout = 'five-row-table';
    const fixedCount = 5;
    const count = document.querySelector('[data-dj-effect-count]');
    const status = document.querySelector('[data-dj-effects-status]');
    const headphone = document.getElementById('headphone-gain');
    const mounts = new Map();
    const incoming = new Map();
    const fields = ['index', 'depth', 'send', 'return', 'enabled', 'monitor', 'cue'];
    const effectCount = template.content.querySelector('[data-fx-selector]')?.options.length || 14;
    let pendingArchive = null;
    let storageError = '';
    let reconciling = false;

    const numberValue = (value, fallback, min, max, step = 1) => {
        const number = typeof value === 'number' || (typeof value === 'string' && value.trim() !== '') ? Number(value) : NaN;
        if (!Number.isFinite(number)) return fallback;
        const bounded = Math.min(max, Math.max(min, number));
        return Number((min + Math.round((bounded - min) / step) * step).toFixed(4));
    };
    const normalize = (raw, id) => ({
        id,
        index: numberValue(raw?.index, 0, 0, effectCount - 1),
        depth: numberValue(raw?.depth, 50, 0, 100),
        send: numberValue(raw?.send, 0, -60, 6, 0.5),
        return: numberValue(raw?.return, -6, -60, 6, 0.5),
        enabled: raw?.enabled === true,
        monitor: ['send', 'return', 'mix'].includes(raw?.monitor) ? raw.monitor : 'return',
        cue: raw?.cue === true
    });
    function restore() {
        let rows = [];
        try {
            const saved = localStorage.getItem(storageKey);
            if (saved !== null) {
                let parsed = null;
                try { parsed = JSON.parse(saved); } catch { /* Preserve an unreadable previous draft before replacing it. */ }
                const savedRows = Array.isArray(parsed) ? parsed : parsed?.effects;
                if (Array.isArray(savedRows)) rows = savedRows.filter((row) => row && typeof row === 'object' && !Array.isArray(row));
                if (parsed?.layout !== layout || rows.length !== fixedCount || rows.some((row, index) => row.id !== `fx-${index + 1}`)) pendingArchive = saved;
            }
        } catch { /* The controls still work when local storage is unavailable. */ }
        return { effects: Array.from({ length: fixedCount }, (_, index) => normalize(rows[index], `fx-${index + 1}`)), nextId: fixedCount + 1 };
    }
    const state = restore();
    const effectName = (effect) => {
        const selector = mounts.get(effect.id)?.section.querySelector('[data-fx-selector]');
        return selector?.selectedOptions[0]?.textContent.trim() || template.content.querySelector('[data-fx-selector]')?.options[effect.index]?.textContent.trim() || 'Delay';
    };
    const snapshot = () => ({
        effects: state.effects.map((effect, index) => {
            const routes = (incoming.get(effect.id) || []).map((channel) => ({ ...channel }));
            return { ...effect, number: index + 1, name: effectName(effect), type: effectName(effect), source: routes.length === 1 ? `mixer:${routes[0].id}` : routes.length ? 'mixer:multiple' : '', incoming: routes, cue: routes.length > 0 && effect.cue };
        }),
        nextId: fixedCount + 1
    });
    function persist() {
        try {
            if (pendingArchive !== null) {
                let key = archiveKey;
                let existing = localStorage.getItem(key);
                let suffix = 1;
                while (existing !== null && existing !== pendingArchive) {
                    key = `${archiveKey}:${++suffix}`;
                    existing = localStorage.getItem(key);
                }
                if (existing === null) localStorage.setItem(key, pendingArchive);
                pendingArchive = null;
            }
            localStorage.setItem(storageKey, JSON.stringify({ version: 2, layout, ...state }));
            storageError = '';
            return true;
        } catch {
            storageError = pendingArchive !== null ? 'Önceki efekt ayarları yedeklenemedi; değişiklikler bu oturumda tutuluyor.' : 'Efekt ayarları bu oturumda tutuluyor; kaydedilemedi.';
            if (status) status.textContent = storageError;
            return false;
        }
    }
    function announce(message) {
        if (count) count.textContent = `${fixedCount} effector`;
        if (status && (storageError || message)) status.textContent = storageError || message;
        document.dispatchEvent(new CustomEvent('radios:dj-effects-change', { detail: snapshot() }));
    }
    function update(effect, patch) {
        const next = normalize({ ...effect, ...patch }, effect.id);
        if (!fields.some((field) => effect[field] !== next[field])) return;
        Object.assign(effect, next);
        persist();
        announce();
    }
    // Template controls are added after knobs.js has completed its static scan.
    function bindKnob(input, signal, onInput, labelForValue) {
        const control = input?.closest('.knob-control');
        const shell = control?.querySelector('.knob-shell');
        if (!input || !control || !shell) return () => {};
        const output = control.querySelector('.knob-value');
        const min = Number(input.min || 0);
        const max = Number(input.max || 100);
        const step = input.step === 'any' ? 0 : Number(input.step || 1);
        const center = Number(input.dataset.center);
        const clamp = (value) => Math.min(max, Math.max(min, value));
        let drag = null;
        function render() {
            const value = Number(input.value);
            let angle = max === min ? 0 : ((value - min) / (max - min)) * 270 - 135;
            if (input.dataset.center !== undefined && min < center && center < max) {
                angle = value <= center ? -135 + (value - min) / (center - min) * 135 : (value - center) / (max - center) * 135;
            }
            const number = Number(value.toFixed(4));
            const unit = input.dataset.unit || '';
            const text = labelForValue ? labelForValue(value) : `${input.dataset.signed !== 'false' && number > 0 ? '+' : ''}${number}${unit ? ` ${unit}` : ''}`;
            control.style.setProperty('--knob-angle', `${angle}deg`);
            if (output) output.value = text;
            input.setAttribute('aria-valuetext', text);
        }
        function setValue(value) {
            const snapped = step > 0 ? min + Math.round((value - min) / step) * step : value;
            const next = String(Number(clamp(snapped).toFixed(4)));
            if (input.value === next) return;
            input.value = next;
            input.dispatchEvent(new Event('input', { bubbles: true }));
        }
        function finish(event) {
            if (!drag || event.pointerId !== drag.pointerId) return;
            const changed = Number(input.value) !== drag.initial;
            drag = null;
            control.classList.remove('is-dragging');
            if (shell.hasPointerCapture(event.pointerId)) shell.releasePointerCapture(event.pointerId);
            if (changed) input.dispatchEvent(new Event('change', { bubbles: true }));
        }
        function reset(event) {
            if (input.disabled) return;
            event.preventDefault();
            const previous = Number(input.value);
            const resetValue = Number(input.dataset.resetValue || 0);
            setValue(Number.isFinite(resetValue) ? resetValue : 0);
            if (previous !== Number(input.value)) input.dispatchEvent(new Event('change', { bubbles: true }));
        }
        ['input', 'change'].forEach((type) => input.addEventListener(type, () => {
            render();
            onInput(Number(input.value));
        }, { signal }));
        input.addEventListener('keydown', (event) => {
            if (event.key.toLowerCase() === 'r' && !event.ctrlKey && !event.metaKey && !event.altKey) reset(event);
        }, { signal });
        shell.style.touchAction = 'none';
        shell.style.userSelect = 'none';
        shell.addEventListener('pointerdown', (event) => {
            if (input.disabled || drag || event.button !== 0) return;
            event.preventDefault();
            input.focus({ preventScroll: true });
            drag = { pointerId: event.pointerId, x: event.clientX, y: event.clientY, value: Number(input.value), initial: Number(input.value) };
            shell.setPointerCapture(event.pointerId);
            control.classList.add('is-dragging');
        }, { signal });
        shell.addEventListener('pointermove', (event) => {
            if (!drag || event.pointerId !== drag.pointerId) return;
            event.preventDefault();
            drag.value = clamp(drag.value + ((event.clientX - drag.x) + (drag.y - event.clientY)) * (max - min) / 180);
            drag.x = event.clientX;
            drag.y = event.clientY;
            setValue(drag.value);
        }, { signal });
        ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((type) => shell.addEventListener(type, finish, { signal }));
        shell.addEventListener('dblclick', reset, { signal });
        signal.addEventListener('abort', () => {
            if (drag && shell.hasPointerCapture(drag.pointerId)) shell.releasePointerCapture(drag.pointerId);
            drag = null;
            control.classList.remove('is-dragging');
        }, { once: true });
        render();
        return render;
    }

    function mount(effect, number) {
        const section = template.content.querySelector('[data-fx-control]')?.cloneNode(true);
        if (!section) return;
        const prefix = `cue-fx-${number}`;
        const idMap = new Map();
        [section, ...section.querySelectorAll('[id]')].forEach((element) => {
            if (!element.id) return;
            const oldId = element.id;
            element.id = `${prefix}-${oldId.replace(/^cue-fx-\d+-/, '')}`;
            idMap.set(oldId, element.id);
        });
        [section, ...section.querySelectorAll('*')].forEach((element) => {
            ['for', 'aria-labelledby', 'aria-describedby', 'aria-controls'].forEach((attribute) => {
                if (!element.hasAttribute(attribute)) return;
                const references = element.getAttribute(attribute).split(/\s+/).map((id) => idMap.get(id) || id);
                element.setAttribute(attribute, references.join(' '));
            });
        });
        section.dataset.fxSlot = String(number);
        section.dataset.djEffectId = effect.id;
        section.setAttribute('aria-label', `Effector ${number}`);
        const numberLabel = section.querySelector('.fx-slot-number');
        if (numberLabel) numberLabel.textContent = String(number);
        const selector = section.querySelector('[data-fx-selector]');
        const depth = section.querySelector('[data-fx-depth]');
        const send = section.querySelector('[data-fx-send]');
        const returnLevel = section.querySelector('[data-fx-return]');
        const monitorPath = section.querySelector('[data-fx-monitor]');
        const cue = section.querySelector('[data-fx-monitor-toggle]');
        const power = section.querySelector('.fx-power-button');
        const incomingOutput = section.querySelector('[data-fx-incoming]');
        incomingOutput?.setAttribute('aria-label', `Effector ${number} · gönderen kanallar`);
        const controller = new AbortController();
        const { signal } = controller;
        selector.value = String(effect.index);
        depth.value = String(effect.depth);
        send.value = String(effect.send);
        returnLevel.value = String(effect.return);
        monitorPath.value = effect.monitor;
        [[selector, 'efekt türü'], [depth, 'derinlik'], [send, 'SEND seviyesi'], [returnLevel, 'RETURN seviyesi'], [monitorPath, 'monitor yolu'], [power, 'efekti aç veya kapat'], [cue, 'seçili monitor için CUE']].forEach(([control, label]) => control?.setAttribute('aria-label', `Effector ${number} · ${label}`));
        rack.append(section);
        let incomingSignature = null;
        function render() {
            const routes = incoming.get(effect.id) || [];
            const hasSource = routes.length > 0;
            const armed = hasSource && effect.cue;
            const signature = JSON.stringify(routes);
            if (incomingOutput && signature !== incomingSignature) {
                incomingSignature = signature;
                if (routes.length) {
                    incomingOutput.replaceChildren(...routes.map((channel) => {
                        const chip = document.createElement('span');
                        chip.className = 'fx-incoming-chip';
                        chip.textContent = channel.label;
                        return chip;
                    }));
                } else incomingOutput.textContent = 'Gönderim yok';
                incomingOutput.title = routes.length ? routes.map((channel) => channel.label).join(', ') : 'Mixer kanalının Effector menüsünden gönderim seç.';
            }
            power.textContent = effect.enabled ? 'ON' : 'OFF';
            power.setAttribute('aria-pressed', String(effect.enabled));
            power.classList.toggle('is-active', effect.enabled);
            const powerState = section.querySelector('.fx-power-state');
            if (powerState) powerState.textContent = effect.enabled ? 'Açık' : 'Kapalı';
            cue.disabled = !hasSource;
            cue.setAttribute('aria-pressed', String(armed));
            cue.title = hasSource ? `Effector ${number} · ${effectName(effect)} · ${effect.monitor.toUpperCase()} CUE (GUI)` : 'Önce mixer kanalından bu effector’e gönderim seç.';
            section.dataset.enabled = String(effect.enabled);
            section.dataset.monitor = String(armed);
            section.dataset.route = effect.monitor;
        }
        [[depth, 'depth'], [send, 'send'], [returnLevel, 'return']].forEach(([input, field]) => bindKnob(input, signal, (value) => {
            update(effect, { [field]: value });
            render();
        }));
        ['input', 'change'].forEach((type) => {
            selector.addEventListener(type, () => {
                update(effect, { index: Number(selector.value) });
                render();
            }, { signal });
            monitorPath.addEventListener(type, () => {
                update(effect, { monitor: monitorPath.value, cue: monitorPath.value === effect.monitor ? effect.cue : false });
                render();
            }, { signal });
        });
        cue.addEventListener('click', () => {
            if (cue.disabled || !(incoming.get(effect.id)?.length)) return;
            update(effect, { cue: !effect.cue });
            render();
        }, { signal });
        power.addEventListener('click', () => {
            update(effect, { enabled: !effect.enabled });
            render();
        }, { signal });
        mounts.set(effect.id, { section, controller, render });
        render();
    }
    function refreshIncoming() {
        // Mixer initialization emits partial snapshots before it publishes its API.
        if (reconciling || !window.RadioSDJMixer?.snapshot) return;
        let mixer;
        try { mixer = window.RadioSDJMixer.snapshot(); } catch { return; }
        if (!mixer || !Array.isArray(mixer.channels)) return;
        reconciling = true;
        try {
            const routes = new Map(state.effects.map((effect) => [effect.id, []]));
            const channels = [...mixer.channels, { id: 'dj-master', label: 'MASTER', effector: mixer.master?.effector }];
            for (const channel of channels) {
                if (!channel || !routes.has(channel.effector)) continue;
                routes.get(channel.effector).push({ id: String(channel.id || ''), label: String(channel.label || channel.id || 'Kanal') });
            }
            let changed = false;
            let cueChanged = false;
            for (const effect of state.effects) {
                const next = routes.get(effect.id);
                if (JSON.stringify(next) !== JSON.stringify(incoming.get(effect.id) || [])) changed = true;
                incoming.set(effect.id, next);
                if (!next.length && effect.cue) {
                    effect.cue = false;
                    cueChanged = true;
                }
                mounts.get(effect.id)?.render();
            }
            if (cueChanged) persist();
            if (changed || cueChanged) announce();
        } finally { reconciling = false; }
    }

    window.RadioSDJEffects = Object.freeze({ snapshot });
    state.effects.forEach((effect, index) => mount(effect, index + 1));
    document.addEventListener('radios:dj-mixer-channels-change', refreshIncoming);
    document.addEventListener('radios:monitor-options-change', () => mounts.forEach((mounted) => mounted.render()));
    ['input', 'change'].forEach((type) => headphone?.addEventListener(type, () => mounts.forEach((mounted) => mounted.render())));
    persist();
    refreshIncoming();
    announce('Mixer kanalında Effector 1–5 seç; gönderimler bu tabloda görünür.');
})();
