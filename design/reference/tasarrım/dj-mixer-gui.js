(() => {
    'use strict';

    const root = document.querySelector('#dj-mixer[data-dj-mixer-gui]');
    const channelList = root?.querySelector('[data-dj-mixer-channels]');
    const masterHost = root?.querySelector('[data-dj-mixer-master]');
    if (!root || !channelList || !masterHost) return;

    const storageKey = 'radios.dj-mixer.v1';
    const effectTypes = { delay: 'Delay', echo: 'Echo', spiral: 'Spiral', reverb: 'Reverb', trans: 'Trans', filter: 'Filter', flanger: 'Flanger', phaser: 'Phaser', robot: 'Robot', melodic: 'Melodic', slipRoll: 'Slip Roll', roll: 'Roll', revRoll: 'Rev Roll', midiLfo: 'MIDI LFO' };
    const channels = new Map();
    const padOwner = (id) => id === 'dj-master' ? master : channels.get(id);
    let sequence = 0;
    let sourceOptions = [];
    let effectorOptions = [];
    let effectorsReady = false;
    let effectorSignature = '';
    let padItems = [];
    let editingPad = null;
    let previousPadFocus = null;
    const cleanText = (value, fallback = '') => typeof value === 'string' && value.trim() ? value.trim() : fallback;
    const create = (tag, className = '', content) => {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (content !== undefined) node.textContent = content;
        return node;
    };
    const makeButton = (className, content, attribute, value = '') => {
        const node = create('button', className, content);
        node.type = 'button';
        if (attribute) node.setAttribute(attribute, value);
        return node;
    };
    const announce = (message) => {
        const status = root.querySelector('[data-dj-mixer-status]');
        if (status) status.textContent = message;
    };
    const dbText = (value) => `${value < 0 ? '−' : value > 0 ? '+' : ''}${Math.abs(value).toLocaleString('tr-TR')} dB`;
    const clamp = (value, min, max, step, fallback) => {
        const numeric = Number(value);
        return Number.isFinite(numeric) ? Math.round(Math.max(min, Math.min(max, numeric)) / step) * step : fallback;
    };
    const clonePad = (pad) => pad && typeof pad === 'object' && cleanText(pad.title) ? {
        title: cleanText(pad.title).slice(0, 120),
        artist: cleanText(pad.artist).slice(0, 120),
        album: cleanText(pad.album).slice(0, 120),
        duration: /^[0-9]{1,3}:[0-5][0-9]$/.test(cleanText(pad.duration)) ? pad.duration.trim() : '',
        origin: cleanText(pad.origin).slice(0, 160),
    } : null;
    const cleanEffector = (value) => /^fx-[1-9]\d*$/.test(cleanText(value)) ? value.trim() : '';
    const channelData = (channel) => ({
        id: channel.id, number: channel.number, label: channel.label, source: channel.source,
        effector: channel.effector,
        send: channel.send, return: channel.return, pan: channel.pan, volume: channel.volume,
        cue: channel.cue, mute: channel.mute, solo: channel.solo,
        pads: channel.pads.map((pad) => pad ? { ...pad } : null),
        selectedPads: [...channel.selectedPads],
    });
    const snapshot = () => ({
        channels: [...channels.values()].map(channelData),
        master: { volume: master.volume, cue: master.cue, mute: master.mute, send: master.send, return: master.return, effector: master.effector, pads: master.pads.map((pad) => pad ? { ...pad } : null), selectedPads: [...master.selectedPads], effect: { ...master.effect } },
    });
    const persist = () => {
        try { localStorage.setItem(storageKey, JSON.stringify({ version: 1, ...snapshot() })); } catch { /* A private session can still use the GUI. */ }
    };
    const channelsChanged = () => {
        const count = root.querySelector('[data-dj-mixer-count]');
        if (count) count.value = `${channels.size} kanal + MASTER · ${(channels.size + 1) * 4} sampler`;
        document.dispatchEvent(new CustomEvent('radios:dj-mixer-channels-change', { detail: snapshot() }));
    };

    const buildMeter = () => {
        const meter = create('div', 'dj-channel-level-meter');
        meter.setAttribute('role', 'img');
        meter.setAttribute('aria-label', 'Stereo sinyal seviyesi: ölçüm bekleniyor');
        const bars = create('div', 'dj-meter-bars');
        bars.setAttribute('aria-hidden', 'true');
        for (let side = 0; side < 2; side += 1) {
            const lane = create('div', 'dj-meter-lane');
            for (let index = 0; index < 20; index += 1) lane.append(create('span', 'dj-meter-segment'));
            bars.append(lane);
        }
        const scale = create('div', 'dj-meter-scale');
        scale.setAttribute('aria-hidden', 'true');
        ['+6', '0', '−12', '−24', '−60'].forEach((mark) => scale.append(create('span', '', mark)));
        meter.append(bars, scale);
        return meter;
    };

    const bindRange = (channel, key, input, control = null, shell = null) => {
        const pan = key === 'pan';
        const depth = key === 'depth';
        const min = depth ? 0 : pan ? -100 : -60;
        const max = depth || pan ? 100 : 6;
        const step = depth || pan ? 1 : 0.5;
        const initial = depth ? 50 : key === 'return' ? -6 : 0;
        const readValue = () => depth ? channel.effect.depth : channel[key];
        const output = channel.refs[`${key}Output`];
        const render = (value) => {
            const current = clamp(value, min, max, step, initial);
            if (depth) channel.effect.depth = current;
            else channel[key] = current;
            input.value = String(current);
            const label = depth ? `${current} %` : pan ? (current === 0 ? 'Merkez' : `${Math.abs(current)} ${current < 0 ? 'L' : 'R'}`) : dbText(current);
            output.value = label;
            input.setAttribute('aria-valuetext', label);
            if (control) {
                const angle = depth ? current / 100 * 270 - 135 : pan ? current / 100 * 135 : current <= 0 ? current / 60 * 135 : current / 6 * 135;
                control.style.setProperty('--knob-angle', `${angle}deg`);
            }
            if (key === 'volume') channel.node.style.setProperty('--dj-volume-percent', `${(channel.volume + 60) / 66 * 100}%`);
        };
        input.addEventListener('input', () => { render(input.value); persist(); });
        input.addEventListener('change', () => { render(input.value); persist(); });
        input.addEventListener('keydown', (event) => {
            if (event.key.toLowerCase() !== 'r' || event.ctrlKey || event.metaKey || event.altKey) return;
            event.preventDefault();
            render(initial);
            persist();
        });
        if (shell) {
            let drag = null;
            shell.style.touchAction = 'none';
            shell.addEventListener('pointerdown', (event) => {
                if (event.button !== 0 || drag) return;
                event.preventDefault();
                input.focus({ preventScroll: true });
                drag = { id: event.pointerId, x: event.clientX, y: event.clientY, value: readValue() };
                shell.setPointerCapture(event.pointerId);
                control.classList.add('is-dragging');
            });
            shell.addEventListener('pointermove', (event) => {
                if (!drag || drag.id !== event.pointerId) return;
                event.preventDefault();
                render(drag.value + (event.clientX - drag.x + drag.y - event.clientY) * (max - min) / 160);
            });
            const endDrag = (event) => {
                if (!drag || drag.id !== event.pointerId) return;
                drag = null;
                control.classList.remove('is-dragging');
                persist();
            };
            ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((name) => shell.addEventListener(name, endDrag));
            shell.addEventListener('dblclick', () => { render(initial); persist(); });
        } else input.addEventListener('dblclick', () => { render(initial); persist(); });
        render(readValue());
    };

    const buildKnob = (channel, key, label) => {
        const control = create('div', `dj-knob dj-knob-${key} knob-control`);
        const inputId = `${channel.id}-${key}`;
        const labelNode = create('label', 'dj-knob-label', label);
        labelNode.htmlFor = inputId;
        const shell = create('div', 'knob-shell');
        const ring = create('div', 'knob-ring');
        const rotor = create('div', 'knob-rotor');
        ring.setAttribute('aria-hidden', 'true');
        rotor.setAttribute('aria-hidden', 'true');
        const input = create('input', 'dj-knob-input');
        input.id = inputId;
        input.type = 'range';
        input.min = key === 'depth' ? '0' : key === 'pan' ? '-100' : '-60';
        input.max = ['pan', 'depth'].includes(key) ? '100' : '6';
        input.step = ['pan', 'depth'].includes(key) ? '1' : '0.5';
        input.dataset.djKnob = key;
        if (key === 'depth') input.dataset.djMasterEffectDepth = '';
        input.setAttribute('aria-label', `${channel.label} ${label}`);
        const output = create('output', 'dj-knob-value');
        output.htmlFor = inputId;
        channel.refs[`${key}Output`] = output;
        shell.append(ring, rotor, input);
        control.append(labelNode, shell, output);
        bindRange(channel, key, input, control, shell);
        return control;
    };

    const buildVolume = (channel) => {
        const group = create('div', 'dj-channel-volume');
        const label = create('label', 'dj-volume-label', 'VOLUME');
        const inputId = `${channel.id}-volume`;
        label.htmlFor = inputId;
        const rails = create('div', 'dj-volume-rails');
        const wrap = create('div', 'dj-fader-wrap');
        const input = create('input', 'dj-fader');
        input.id = inputId;
        input.type = 'range';
        input.min = '-60';
        input.max = '6';
        input.step = '0.5';
        input.setAttribute('orient', 'vertical');
        input.setAttribute('aria-orientation', 'vertical');
        input.setAttribute('aria-label', `${channel.label} volume`);
        input.dataset.djFader = '';
        const scale = create('div', 'dj-fader-scale');
        scale.setAttribute('aria-hidden', 'true');
        ['+6', '0', '−12', '−24', '−48', '−60'].forEach((value) => scale.append(create('span', '', value)));
        wrap.append(scale, input);
        rails.append(wrap, buildMeter());
        const output = create('output', 'dj-channel-volume-output');
        output.htmlFor = inputId;
        channel.refs.volumeOutput = output;
        group.append(label, rails, output);
        bindRange(channel, 'volume', input);
        return group;
    };

    const buildSwitches = (channel, masterChannel = false) => {
        const group = create('div', 'dj-channel-switches');
        group.setAttribute('role', 'group');
        group.setAttribute('aria-label', `${channel.label} monitor kontrolleri`);
        [['cue', 'CUE'], ['mute', 'M'], ...(masterChannel ? [] : [['solo', 'S']])].forEach(([key, label]) => {
            const control = makeButton(`dj-channel-switch dj-switch-${key}`, label, 'data-dj-switch', key);
            if (key === 'cue') control.dataset.djCue = '';
            control.setAttribute('aria-label', `${channel.label} ${key === 'cue' ? 'kulaklık CUE' : key === 'mute' ? 'mute' : 'solo'}`);
            const render = () => {
                control.setAttribute('aria-pressed', String(channel[key]));
                control.classList.toggle('is-active', channel[key]);
                channel.node.classList.toggle(`is-${key}`, channel[key]);
            };
            control.addEventListener('click', () => {
                channel[key] = !channel[key];
                render();
                persist();
                announce(`${channel.label}: ${key.toUpperCase()} ${channel[key] ? 'seçili' : 'kapalı'}.`);
            });
            render();
            group.append(control);
        });
        return group;
    };

    const renderPads = (channel) => {
        channel.refs.pads.forEach((refs, index) => {
            const pad = channel.pads[index];
            const selected = Boolean(pad && channel.selectedPads.has(index));
            refs.title.textContent = pad?.title || 'Boş';
            refs.title.title = pad?.title || 'Sampler içeriği ata';
            refs.state.textContent = selected ? 'Seçili' : pad ? 'Atandı' : '';
            refs.button.classList.toggle('is-assigned', Boolean(pad));
            refs.button.classList.toggle('is-active', selected);
            refs.button.setAttribute('aria-pressed', String(selected));
            refs.button.setAttribute('aria-label', `${channel.label} sampler ${index + 1}: ${pad?.title || 'boş, içerik ata'}${selected ? ', seçili' : ''}`);
            if (pad) {
                refs.button.removeAttribute('aria-haspopup');
                refs.button.removeAttribute('aria-controls');
            } else {
                refs.button.setAttribute('aria-haspopup', 'dialog');
                refs.button.setAttribute('aria-controls', 'dj-sampler-dialog');
            }
        });
    };

    const padDialog = create('dialog', 'ops-dialog dj-sampler-dialog');
    padDialog.id = 'dj-sampler-dialog';
    padDialog.setAttribute('aria-labelledby', 'dj-sampler-dialog-title');
    const padForm = create('form', 'dj-sampler-form');
    const dialogHead = create('header', 'dj-sampler-dialog-heading');
    const dialogTitle = create('h2', '', 'Sampler içeriği');
    dialogTitle.id = 'dj-sampler-dialog-title';
    const closeDialog = makeButton('dj-dialog-close', '×', 'data-dj-pad-close');
    closeDialog.setAttribute('aria-label', 'Sampler düzenleyicisini kapat');
    dialogHead.append(dialogTitle, closeDialog);
    const fields = create('div', 'dj-sampler-dialog-fields');
    const libraryLabel = create('label', '', 'Program / playlist girdisi');
    libraryLabel.htmlFor = 'dj-sampler-item';
    const itemSelect = create('select');
    itemSelect.id = 'dj-sampler-item';
    const titleLabel = create('label', '', 'Sampler başlığı');
    titleLabel.htmlFor = 'dj-sampler-title';
    const titleInput = create('input');
    titleInput.id = 'dj-sampler-title';
    titleInput.type = 'text';
    titleInput.maxLength = 120;
    titleInput.required = true;
    titleInput.placeholder = 'Başlık';
    fields.append(libraryLabel, itemSelect, titleLabel, titleInput);
    const actions = create('div', 'dj-dialog-actions');
    const clearPad = makeButton('dj-dialog-clear', 'Atamayı kaldır', 'data-dj-pad-clear');
    const cancelPad = makeButton('', 'Vazgeç', 'data-dj-pad-close');
    const savePad = create('button', 'task-primary', 'Ata');
    savePad.type = 'submit';
    actions.append(clearPad, cancelPad, savePad);
    padForm.append(dialogHead, fields, actions);
    padDialog.append(padForm);
    root.append(padDialog);

    const closePadDialog = () => {
        padDialog.close();
    };
    padDialog.addEventListener('close', () => {
        editingPad = null;
        if (previousPadFocus?.isConnected) previousPadFocus.focus({ preventScroll: true });
        previousPadFocus = null;
    });
    padDialog.querySelectorAll('[data-dj-pad-close]').forEach((button) => button.addEventListener('click', closePadDialog));
    const renderPadOptions = () => {
        const previous = itemSelect.value;
        const placeholder = create('option', '', 'Girdi seç');
        placeholder.value = '';
        const fragment = document.createDocumentFragment();
        fragment.append(placeholder);
        const groups = new Map();
        padItems.forEach((item) => {
            if (!groups.has(item.group)) {
                const group = create('optgroup');
                group.label = item.group;
                groups.set(item.group, group);
                fragment.append(group);
            }
            const option = create('option', '', `${item.pad.title}${item.pad.artist ? ` · ${item.pad.artist}` : ''}`);
            option.value = item.id;
            groups.get(item.group).append(option);
        });
        itemSelect.replaceChildren(fragment);
        itemSelect.value = padItems.some((item) => item.id === previous) ? previous : '';
    };
    const openPadDialog = (channel, index, originButton) => {
        editingPad = { channelId: channel.id, index, original: clonePad(channel.pads[index]) };
        previousPadFocus = originButton;
        dialogTitle.textContent = `${channel.label} · Sampler ${index + 1}`;
        renderPadOptions();
        itemSelect.value = '';
        titleInput.value = channel.pads[index]?.title || '';
        titleInput.setCustomValidity('');
        clearPad.disabled = !channel.pads[index];
        padDialog.showModal();
        itemSelect.focus();
    };
    itemSelect.addEventListener('change', () => {
        const selected = padItems.find((item) => item.id === itemSelect.value);
        if (selected) { titleInput.value = selected.pad.title; titleInput.setCustomValidity(''); }
    });
    titleInput.addEventListener('input', () => titleInput.setCustomValidity(''));
    padForm.addEventListener('submit', (event) => {
        event.preventDefault();
        const channel = padOwner(editingPad?.channelId);
        const title = cleanText(titleInput.value);
        if (!channel) return;
        titleInput.setCustomValidity(title ? '' : 'Sampler başlığı gir.');
        if (!padForm.reportValidity()) return;
        const selected = padItems.find((item) => item.id === itemSelect.value);
        channel.pads[editingPad.index] = { ...(selected ? selected.pad : editingPad.original || { artist: '', album: '', duration: '', origin: '' }), title: title.slice(0, 120) };
        channel.selectedPads.delete(editingPad.index);
        renderPads(channel);
        persist();
        announce(`${channel.label} sampler ${editingPad.index + 1}: ${title} atandı.`);
        closePadDialog();
    });
    clearPad.addEventListener('click', () => {
        const channel = padOwner(editingPad?.channelId);
        if (!channel) return;
        channel.pads[editingPad.index] = null;
        channel.selectedPads.delete(editingPad.index);
        renderPads(channel);
        persist();
        announce(`${channel.label} sampler ${editingPad.index + 1} ataması kaldırıldı.`);
        closePadDialog();
    });

    const buildPads = (channel) => {
        const group = create('div', 'dj-channel-pads');
        group.setAttribute('role', 'group');
        group.setAttribute('aria-label', `${channel.label} dört sampler`);
        channel.refs.pads = [];
        for (let index = 0; index < 4; index += 1) {
            const wrap = create('div', 'dj-pad');
            const pad = makeButton('dj-sampler-pad', undefined, 'data-dj-sampler-pad', String(index + 1));
            const number = create('span', 'dj-pad-number', String(index + 1).padStart(2, '0'));
            const title = create('span', 'dj-pad-title');
            const state = create('span', 'dj-pad-state');
            pad.append(number, title, state);
            const edit = makeButton('dj-pad-edit', '⋯', 'data-dj-pad-edit', String(index + 1));
            edit.title = `${channel.label} sampler ${index + 1} içeriğini düzenle`;
            edit.setAttribute('aria-label', edit.title);
            edit.setAttribute('aria-haspopup', 'dialog');
            edit.setAttribute('aria-controls', padDialog.id);
            pad.addEventListener('click', () => {
                if (!channel.pads[index]) { openPadDialog(channel, index, pad); return; }
                if (channel.selectedPads.has(index)) channel.selectedPads.delete(index);
                else channel.selectedPads.add(index);
                renderPads(channel);
                persist();
                announce(`${channel.label} sampler ${index + 1}: ${channel.pads[index].title} ${channel.selectedPads.has(index) ? 'seçili' : 'seçimi kaldırıldı'}.`);
            });
            edit.addEventListener('click', () => openPadDialog(channel, index, edit));
            wrap.append(pad, edit);
            group.append(wrap);
            channel.refs.pads.push({ button: pad, title, state });
        }
        renderPads(channel);
        return group;
    };

    const renderChannelSources = (channel) => {
        const placeholder = create('option', '', 'Kaynak seç');
        placeholder.value = '';
        const fragment = document.createDocumentFragment();
        fragment.append(placeholder);
        const groups = new Map();
        sourceOptions.forEach((source) => {
            if (!groups.has(source.group)) {
                const group = create('optgroup');
                group.label = source.group;
                groups.set(source.group, group);
                fragment.append(group);
            }
            const option = create('option', '', source.label);
            option.value = source.id;
            groups.get(source.group).append(option);
        });
        if (channel.source && !sourceOptions.some((source) => source.id === channel.source)) {
            const unavailable = create('option', '', 'Önceki kaynak · listede yok');
            unavailable.value = channel.source;
            fragment.append(unavailable);
        }
        channel.refs.source.replaceChildren(fragment);
        channel.refs.source.value = channel.source;
    };
    const renderEffectorOptions = (channel) => {
        const select = channel.refs.effector;
        if (!select) return false;
        if (channel.refs.effectorSignature === effectorSignature && select.value === channel.effector) return false;
        const fragment = document.createDocumentFragment();
        const none = create('option', '', 'Yok');
        none.value = '';
        fragment.append(none);
        effectorOptions.forEach((effect) => {
            const option = create('option', '', effect.label);
            option.value = effect.id;
            fragment.append(option);
        });
        let cleared = false;
        if (channel.effector && !effectorOptions.some((effect) => effect.id === channel.effector)) {
            if (effectorsReady) {
                channel.effector = '';
                cleared = true;
            } else {
                const pending = create('option', '', `${channel.effector.toUpperCase()} · yükleniyor`);
                pending.value = channel.effector;
                fragment.append(pending);
            }
        }
        select.replaceChildren(fragment);
        select.value = channel.effector;
        channel.refs.effectorSignature = effectorSignature;
        return cleared;
    };
    const refreshEffectors = () => {
        let effects = null;
        try { effects = window.RadioSDJEffects?.snapshot?.()?.effects; } catch { /* Saved routes wait until the effect table is ready. */ }
        if (!Array.isArray(effects)) return;
        effectorsReady = true;
        const names = Object.values(effectTypes);
        effectorOptions = effects.filter((effect) => cleanEffector(effect?.id)).map((effect, index) => ({
            id: effect.id,
            label: `FX ${index + 1} · ${cleanText(effect.name) || effectTypes[effect.type] || names[Number(effect.index)] || 'Efekt'}`,
        }));
        effectorSignature = JSON.stringify(effectorOptions);
        let routesCleared = renderEffectorOptions(master);
        channels.forEach((channel) => { if (renderEffectorOptions(channel)) routesCleared = true; });
        if (routesCleared) { persist(); channelsChanged(); }
    };
    const buildEffector = (channel) => {
        const group = create('div', 'dj-channel-source dj-channel-effector');
        const label = create('label', '', 'Effector');
        const select = create('select');
        select.id = `${channel.id}-effector`;
        select.dataset.djChannelEffector = '';
        if (channel.id === 'dj-master') select.dataset.djMasterEffector = '';
        select.setAttribute('aria-label', `${channel.label} effector seçimi`);
        label.htmlFor = select.id;
        group.append(label, select);
        channel.refs.effector = select;
        renderEffectorOptions(channel);
        select.addEventListener('change', () => {
            if (channel.effector === select.value) return;
            channel.effector = select.value;
            persist();
            channelsChanged();
            announce(`${channel.label}: ${channel.effector ? select.selectedOptions[0].textContent : 'effector seçimi kaldırıldı'}.`);
        });
        return group;
    };
    const refreshSources = () => {
        sourceOptions = [];
        padItems = [];
        let programme = null;
        let programmeItems = [];
        let playlists = [];
        try { const snapshot = window.RadioSProgramme?.getSnapshot?.(); programme = snapshot?.programme; programmeItems = snapshot?.playbackItems || programme?.items || []; } catch { /* The mixer remains available with a missing source. */ }
        try { playlists = window.RadioSPlaylist?.snapshot?.()?.playlists || []; } catch { /* The mixer remains available with a missing source. */ }
        const addItems = (items, group, prefix) => {
            if (!Array.isArray(items)) return;
            items.forEach((item, index) => {
                const pad = clonePad(item);
                if (pad) padItems.push({ id: `${prefix}:${index}`, group, pad: { ...pad, origin: group } });
            });
        };
        document.querySelectorAll('#players-workspace [data-live-player]').forEach((player) => {
            const label = cleanText(player.querySelector('h3')?.textContent, 'Canlı oynatıcı');
            sourceOptions.push({ id: `live-player:${player.dataset.livePlayer}`, label, group: 'Canlı oynatıcılar' });
        });
        if (programme && Array.isArray(programme.items)) {
            const name = cleanText(programme.name, 'Program');
            sourceOptions.push({ id: 'programme', label: name, group: 'Programlar' });
            addItems(programmeItems, name, 'programme');
        }
        if (Array.isArray(playlists)) playlists.forEach((playlist, index) => {
            if (!playlist || !Array.isArray(playlist.items)) return;
            const id = cleanText(playlist.id, String(index));
            const name = cleanText(playlist.name, 'Adsız playlist');
            sourceOptions.push({ id: `playlist:${id}`, label: name, group: 'Playlistler' });
            addItems(playlist.items, name, `playlist:${id}`);
        });
        sourceOptions.push({ id: 'microphone', label: 'Stüdyo mikrofonu', group: 'Stüdyo kaynakları' });
        sourceOptions.push({ id: 'jingle', label: 'Jingle bankası', group: 'Stüdyo kaynakları' });
        channels.forEach(renderChannelSources);
        if (padDialog.open) renderPadOptions();
    };

    const addChannel = (saved = {}, persistChange = true) => {
        const savedNumber = Number(saved.number);
        const number = Number.isSafeInteger(savedNumber) && savedNumber > 0 && !channels.has(`dj-channel-${savedNumber}`) ? savedNumber : sequence + 1;
        sequence = Math.max(sequence, number);
        const id = `dj-channel-${number}`;
        const label = cleanText(saved.label, `Kanal ${number}`).slice(0, 70);
        const channel = {
            id, number, label, source: cleanText(saved.source).slice(0, 200),
            effector: cleanEffector(saved.effector),
            send: clamp(saved.send ?? 0, -60, 6, 0.5, 0),
            return: clamp(saved.return ?? -6, -60, 6, 0.5, -6),
            pan: clamp(saved.pan ?? 0, -100, 100, 1, 0),
            volume: clamp(saved.volume ?? 0, -60, 6, 0.5, 0),
            cue: false, mute: saved.mute === true, solo: saved.solo === true,
            pads: Array.from({ length: 4 }, (_, index) => clonePad(saved.pads?.[index])),
            selectedPads: new Set(), refs: {},
            node: create('article', 'dj-channel'),
        };
        channel.node.dataset.djChannel = id;
        channel.node.setAttribute('aria-labelledby', `${id}-title`);
        const heading = create('header', 'dj-channel-heading');
        const numberNode = create('span', 'dj-channel-number', String(number).padStart(2, '0'));
        const title = create('h3', '', label);
        title.id = `${id}-title`;
        const remove = makeButton('dj-channel-remove', '×', 'data-dj-channel-remove');
        remove.setAttribute('aria-label', `${label} kanalını kaldır`);
        remove.addEventListener('click', () => {
            if (editingPad?.channelId === id) closePadDialog();
            channels.delete(id);
            channel.node.remove();
            persist();
            channelsChanged();
            announce(`${label} kaldırıldı.`);
            root.querySelector('[data-dj-mixer-add]')?.focus({ preventScroll: true });
        });
        heading.append(numberNode, title, remove);
        const sourceBox = create('div', 'dj-channel-source');
        const sourceLabel = create('label', '', 'Kaynak');
        sourceLabel.htmlFor = `${id}-source`;
        const sourceSelect = create('select');
        sourceSelect.id = `${id}-source`;
        sourceSelect.dataset.djChannelSource = '';
        sourceSelect.setAttribute('aria-label', `${label} kaynak`);
        sourceBox.append(sourceLabel, sourceSelect);
        channel.refs.source = sourceSelect;
        sourceSelect.addEventListener('change', () => {
            channel.source = sourceSelect.value;
            persist();
            channelsChanged();
            announce(`${label}: ${sourceSelect.selectedOptions[0]?.textContent || 'kaynak seçilmedi'}.`);
        });
        const buses = create('div', 'dj-channel-buses');
        buses.append(buildKnob(channel, 'send', 'SEND'), buildKnob(channel, 'return', 'RETURN'));
        channel.node.append(heading, create('p', 'dj-sampler-label', 'SAMPLER'), buildPads(channel), sourceBox, buildEffector(channel), buses, buildKnob(channel, 'pan', 'PAN'), buildVolume(channel), buildSwitches(channel));
        channels.set(id, channel);
        renderChannelSources(channel);
        channelList.append(channel.node);
        if (persistChange) { persist(); channelsChanged(); announce(`${label} eklendi. Dört sampler hazır.`); }
        return channel;
    };

    let restored = null;
    try {
        const parsed = JSON.parse(localStorage.getItem(storageKey) || 'null');
        if (parsed?.version === 1 && Array.isArray(parsed.channels) && parsed.channels.every((channel) => channel && typeof channel === 'object')) restored = parsed;
    } catch { /* A corrupt local draft falls back to the default mixer. */ }
    const master = {
        id: 'dj-master', label: 'Master',
        volume: clamp(restored?.master?.volume ?? 0, -60, 6, 0.5, 0),
        cue: restored?.master?.cue === true, mute: restored?.master?.mute === true, refs: {},
        send: clamp(restored?.master?.send ?? 0, -60, 6, 0.5, 0),
        return: clamp(restored?.master?.return ?? -6, -60, 6, 0.5, -6),
        effector: cleanEffector(restored?.master?.effector),
        pads: Array.from({ length: 4 }, (_, index) => clonePad(restored?.master?.pads?.[index])),
        selectedPads: new Set(),
        effect: { type: Object.hasOwn(effectTypes, restored?.master?.effect?.type) ? restored.master.effect.type : 'delay', depth: clamp(restored?.master?.effect?.depth ?? 50, 0, 100, 1, 50), enabled: restored?.master?.effect?.enabled === true },
        node: masterHost,
    };
    if (Array.isArray(restored?.master?.selectedPads)) master.selectedPads = new Set(restored.master.selectedPads.filter((index) => Number.isInteger(index) && index >= 0 && index < 4 && master.pads[index]));
    masterHost.classList.add('dj-channel', 'dj-master-channel');
    masterHost.setAttribute('aria-labelledby', 'dj-master-title');
    const masterHeading = create('header', 'dj-channel-heading');
    const masterTitle = create('h3', '', 'MASTER');
    masterTitle.id = 'dj-master-title';
    masterHeading.append(create('span', 'dj-channel-number', 'M'), masterTitle);
    const masterEffectSource = create('div', 'dj-channel-source dj-master-effect-source');
    masterEffectSource.dataset.djMasterEffect = '';
    const masterEffectLabel = create('label', '', 'EFFECT');
    masterEffectLabel.htmlFor = 'dj-master-effect-type';
    const masterEffectSelect = create('select');
    masterEffectSelect.id = 'dj-master-effect-type';
    masterEffectSelect.dataset.djMasterEffectType = '';
    masterEffectSelect.setAttribute('aria-label', 'Master efekt türü');
    Object.entries(effectTypes).forEach(([value, label]) => { const option = create('option', '', label); option.value = value; masterEffectSelect.append(option); });
    masterEffectSelect.value = master.effect.type;
    masterEffectSelect.addEventListener('change', () => { master.effect.type = masterEffectSelect.value; persist(); announce(`Master efekt: ${effectTypes[master.effect.type]}.`); });
    masterEffectSource.append(masterEffectLabel, masterEffectSelect);
    const masterBuses = create('div', 'dj-channel-buses');
    masterBuses.append(buildKnob(master, 'send', 'SEND'), buildKnob(master, 'return', 'RETURN'));
    const masterEffectControls = create('div', 'dj-master-effect-controls');
    const masterEffectToggle = makeButton('dj-master-effect-toggle', '', 'data-dj-master-effect-enabled');
    masterEffectToggle.setAttribute('aria-label', 'Master efekt aç / kapat');
    const renderMasterEffect = () => { masterEffectToggle.textContent = master.effect.enabled ? 'ON' : 'OFF'; masterEffectToggle.setAttribute('aria-pressed', String(master.effect.enabled)); masterEffectToggle.classList.toggle('is-active', master.effect.enabled); };
    masterEffectToggle.addEventListener('click', () => { master.effect.enabled = !master.effect.enabled; renderMasterEffect(); persist(); announce(`Master efekt ${master.effect.enabled ? 'seçili' : 'kapalı'}.`); });
    renderMasterEffect();
    masterEffectControls.append(buildKnob(master, 'depth', 'DEPTH'), masterEffectToggle);
    masterHost.replaceChildren(masterHeading, create('p', 'dj-sampler-label', 'SAMPLER'), buildPads(master), masterEffectSource, buildEffector(master), masterBuses, masterEffectControls, buildVolume(master), buildSwitches(master, true));

    refreshSources();
    if (restored) restored.channels.forEach((saved) => addChannel(saved, false));
    else {
        const players = sourceOptions.filter((source) => source.group === 'Canlı oynatıcılar');
        for (let index = 0; index < 6; index += 1) addChannel({ label: `Player ${index + 1}`, source: players[index]?.id || '' }, false);
        addChannel({ label: 'Mikrofon', source: 'microphone' }, false);
        addChannel({ label: 'Jingle', source: 'jingle' }, false);
    }
    window.RadioSDJMixer = Object.freeze({ snapshot });
    refreshEffectors();
    channelsChanged();
    announce('');
    root.querySelector('[data-dj-mixer-add]')?.addEventListener('click', () => addChannel());
    ['radios:programme-change', 'radios:playlist-change'].forEach((name) => document.addEventListener(name, refreshSources));
    document.addEventListener('radios:dj-effects-change', refreshEffectors);
    const playersList = document.querySelector('#players-workspace [data-live-player-list]');
    if (playersList) new MutationObserver(refreshSources).observe(playersList, { childList: true });
})();
