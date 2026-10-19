(() => {
    'use strict';

    const workspace = document.querySelector('#players-workspace [data-live-player-gui]');
    const list = workspace?.querySelector('[data-live-player-list]');
    if (!workspace || !list) return;

    const mime = 'application/x-radios-programme-items';
    const cards = new Map();
    const typeLabels = { music: 'Müzik', announcement: 'Anons', advert: 'Reklam', jingle: 'Jingle' };
    const stateLabels = { idle: 'Beklemede', started: 'Başlatıldı', paused: 'Duraklatıldı', finished: 'Tamamlandı' };
    let cardSequence = 0;
    let inputSequence = 0;
    let sources = [];

    const text = (value, fallback = '') => typeof value === 'string' && value.trim() ? value.trim() : fallback;
    const announce = (message) => {
        const status = workspace.querySelector('[data-live-player-status]');
        if (status) status.textContent = message;
    };
    const element = (tag, className, content) => {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (content !== undefined) node.textContent = content;
        return node;
    };
    const button = (className, content, attribute, value = '') => {
        const node = element('button', className, content);
        node.type = 'button';
        node.setAttribute(attribute, value);
        return node;
    };
    const labelledButton = (className, content, attribute, value, label) => {
        const node = button(className, content, attribute, value);
        node.setAttribute('aria-label', label);
        node.title = label;
        return node;
    };
    const copyInputs = (items) => Array.isArray(items) ? items.flatMap((item) => {
        if (!item || typeof item !== 'object' || !text(item.title)) return [];
        return [{
            id: `lp-input-${++inputSequence}`,
            title: text(item.title),
            artist: text(item.artist),
            album: text(item.album),
            duration: /^[0-9]{1,3}:[0-5][0-9]$/.test(text(item.duration)) ? item.duration.trim() : '',
            type: Object.hasOwn(typeLabels, item.type) ? item.type : 'music',
        }];
    }) : [];

    const renderSources = (card) => {
        const previous = card.refs.source.value;
        const fragment = document.createDocumentFragment();
        const placeholder = element('option', '', 'Program / playlist seç');
        placeholder.value = '';
        fragment.append(placeholder);
        ['Programlar', 'Playlistler'].forEach((label) => {
            const groupSources = sources.filter((source) => source.group === label);
            if (!groupSources.length) return;
            const group = element('optgroup');
            group.label = label;
            groupSources.forEach((source) => {
                const option = element('option', '', `${source.name} · ${source.items.length} girdi`);
                option.value = source.id;
                option.disabled = !source.items.length;
                group.append(option);
            });
            fragment.append(group);
        });
        card.refs.source.replaceChildren(fragment);
        card.refs.source.value = sources.some((source) => source.id === previous && source.items.length) ? previous : '';
        card.refs.addInput.disabled = !card.refs.source.value;
    };
    const refreshSources = () => {
        const snapshot = window.RadioSProgramme?.getSnapshot?.();
        const programme = snapshot?.programme;
        const playlists = window.RadioSPlaylist?.snapshot?.()?.playlists;
        sources = [];
        if (programme && Array.isArray(programme.items)) {
            sources.push({ id: 'programme', name: text(programme.name, 'Program taslağı'), group: 'Programlar', items: snapshot.playbackItems || programme.items });
        }
        if (Array.isArray(playlists)) playlists.forEach((playlist, index) => {
            if (!playlist || !Array.isArray(playlist.items)) return;
            sources.push({ id: `playlist:${text(playlist.id, String(index))}`, name: text(playlist.name, 'Adsız playlist'), group: 'Playlistler', items: playlist.items });
        });
        cards.forEach(renderSources);
    };

    const renderVolume = (card, value) => {
        const snapped = Math.round(Math.max(-60, Math.min(6, Number(value) || 0)) * 2) / 2;
        card.volume = snapped;
        card.refs.line.value = String(snapped);
        card.refs.knob.value = String(snapped);
        const signed = `${snapped < 0 ? '−' : snapped > 0 ? '+' : ''}${Math.abs(snapped).toLocaleString('tr-TR')} dB`;
        card.refs.volume.value = signed;
        card.refs.line.setAttribute('aria-valuetext', signed);
        card.refs.knob.setAttribute('aria-valuetext', signed);
        const angle = snapped <= 0 ? snapped / 60 * 135 : snapped / 6 * 135;
        card.refs.knobControl.style.setProperty('--knob-angle', `${angle}deg`);
        const percent = `${(snapped + 60) / 66 * 100}%`;
        card.node.style.setProperty('--lp-volume-percent', percent);
        card.node.style.setProperty('--lp-level-percent', percent);
    };
    const bindVolume = (card) => {
        const { line, knob, shell, knobControl } = card.refs;
        let drag = null;
        [line, knob].forEach((input) => {
            input.addEventListener('input', () => renderVolume(card, input.value));
            input.addEventListener('change', () => renderVolume(card, input.value));
            input.addEventListener('keydown', (event) => {
                if (event.key.toLowerCase() !== 'r' || event.ctrlKey || event.altKey || event.metaKey) return;
                event.preventDefault();
                renderVolume(card, 0);
            });
        });
        shell.style.touchAction = 'none';
        shell.style.userSelect = 'none';
        shell.addEventListener('pointerdown', (event) => {
            if (event.button !== 0 || drag) return;
            event.preventDefault();
            knob.focus({ preventScroll: true });
            drag = { pointerId: event.pointerId, x: event.clientX, y: event.clientY, value: card.volume };
            shell.setPointerCapture(event.pointerId);
            knobControl.classList.add('is-dragging');
        });
        shell.addEventListener('pointermove', (event) => {
            if (!drag || drag.pointerId !== event.pointerId) return;
            event.preventDefault();
            const distance = event.clientX - drag.x + drag.y - event.clientY;
            renderVolume(card, drag.value + distance * 66 / 180);
        });
        const finish = (event) => {
            if (!drag || drag.pointerId !== event.pointerId) return;
            drag = null;
            knobControl.classList.remove('is-dragging');
            if (shell.hasPointerCapture(event.pointerId)) shell.releasePointerCapture(event.pointerId);
        };
        ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((name) => shell.addEventListener(name, finish));
        shell.addEventListener('dblclick', (event) => { event.preventDefault(); renderVolume(card, 0); });
        renderVolume(card, 0);
    };

    const renderCard = (card) => {
        const current = card.queue[card.index];
        const next = card.queue[card.index + 1];
        const hasCurrent = Boolean(current);
        card.refs.state.textContent = stateLabels[card.state];
        card.node.dataset.lpState = card.state;
        card.refs.current.textContent = current?.title || (card.state === 'finished' ? 'Girdiler tamamlandı' : '—');
        card.refs.meta.textContent = current ? [current.artist, current.album, typeLabels[current.type]].filter(Boolean).join(' · ') : '';
        card.refs.next.textContent = next?.title || '—';
        [card.refs.current, card.refs.meta, card.refs.next].forEach((entry) => { entry.title = entry.textContent; });
        card.refs.duration.textContent = current?.duration || '—:—';
        card.refs.elapsed.textContent = '—:—';
        card.refs.count.textContent = `(${card.queue.length})`;
        card.refs.play.disabled = !card.queue.length || card.state === 'started';
        card.refs.pause.disabled = card.state !== 'started';
        card.refs.play.setAttribute('aria-pressed', String(card.state === 'started'));
        card.refs.pause.setAttribute('aria-pressed', String(card.state === 'paused'));
        card.refs.stop.disabled = !card.queue.length;
        card.refs.advance.disabled = !hasCurrent;
        card.refs.cue.disabled = !hasCurrent;
        if (!hasCurrent) card.cue = false;
        card.refs.cue.setAttribute('aria-pressed', String(card.cue));
        card.refs.clear.disabled = !card.queue.length;
        const fragment = document.createDocumentFragment();
        card.queue.forEach((item, index) => {
            const entry = element('li', `lp-input${index === card.index ? ' is-current' : ''}`);
            entry.dataset.lpInput = item.id;
            if (index === card.index) entry.setAttribute('aria-current', 'true');
            const content = element('span', 'lp-input-info');
            content.append(element('strong', '', `${index + 1}. ${item.title}`));
            content.append(element('small', '', [item.artist, item.album, item.duration || 'Süre bilinmiyor'].filter(Boolean).join(' · ')));
            entry.append(content, labelledButton('lp-input-remove', '×', 'data-lp-remove-input', item.id, `${card.name}: ${item.title} girdisini kaldır`));
            fragment.append(entry);
        });
        if (!card.queue.length) fragment.append(element('li', 'lp-input-empty', 'Girdi yok'));
        card.refs.inputs.replaceChildren(fragment);
    };
    const appendInputs = (card, items, sourceName = '') => {
        const copied = copyInputs(items);
        if (!copied.length) { announce(`${card.name}: Eklenecek girdi bulunamadı.`); return; }
        card.queue = card.queue.concat(copied);
        if (card.state === 'finished') card.state = 'idle';
        renderCard(card);
        announce(`${card.name}: ${sourceName ? `${sourceName}, ` : ''}${copied.length} girdi eklendi.`);
    };

    const range = (id, className, attribute, label) => {
        const input = element('input', className);
        input.type = 'range';
        input.id = id;
        input.min = '-60';
        input.max = '6';
        input.step = '0.5';
        input.value = '0';
        input.setAttribute(attribute, '');
        input.setAttribute('aria-label', label);
        return input;
    };
    const createCard = () => {
        const number = ++cardSequence;
        const id = `live-player-${number}`;
        const name = `Oynatıcı ${number}`;
        const node = element('article', 'lp-card');
        node.dataset.livePlayer = id;
        node.setAttribute('aria-labelledby', `${id}-title`);
        const numberBadge = element('span', 'lp-card-number', String(number));
        numberBadge.setAttribute('aria-hidden', 'true');
        const body = element('div', 'lp-body');
        const controls = element('div', 'lp-controls');
        const heading = element('header', 'lp-card-heading');
        const title = element('h3', '', name);
        title.id = `${id}-title`;
        const state = element('span', 'lp-state');
        heading.append(title, state,
            labelledButton('lp-remove', '×', 'data-lp-remove', '', `${name} kartını kaldır`));

        const sourceBlock = element('div', 'lp-source');
        const sourceLabel = element('label', '', 'Program / playlist');
        sourceLabel.htmlFor = `${id}-source`;
        const sourceRow = element('div', 'lp-source-row');
        const source = element('select');
        source.id = `${id}-source`;
        source.setAttribute('data-lp-source', '');
        const addInput = labelledButton('lp-add-input', 'Girdi ekle', 'data-lp-add-input', '', `${name} kartına seçilen kaynağın girdilerini ekle`);
        sourceRow.append(source, addInput);
        sourceBlock.append(sourceLabel, sourceRow);

        const currentBlock = element('div', 'lp-current');
        const current = element('p', 'lp-current-title');
        current.setAttribute('data-lp-current-title', '');
        const meta = element('p', 'lp-current-meta');
        meta.setAttribute('data-lp-current-meta', '');
        const nextRow = element('p', 'lp-next');
        const next = element('span');
        next.setAttribute('data-lp-next-title', '');
        nextRow.append(element('span', '', 'Sıradaki'), next);
        currentBlock.append(current, meta, nextRow);
        const times = element('dl', 'lp-times');
        const elapsed = element('dd', '', '—:—');
        elapsed.setAttribute('aria-label', `${name}: Geçen süre bilinmiyor`);
        const duration = element('dd', '', '—:—');
        const elapsedGroup = element('div');
        elapsedGroup.append(element('dt', '', 'Geçen'), elapsed);
        const durationGroup = element('div');
        durationGroup.append(element('dt', '', 'Süre'), duration);
        times.append(elapsedGroup, durationGroup);

        const transport = element('div', 'lp-transport');
        transport.setAttribute('role', 'group');
        transport.setAttribute('aria-label', `${name} kontrolleri`);
        const play = labelledButton('', '▶ Oynat', 'data-lp-transport', 'play', `${name}: başlat`);
        const pause = labelledButton('', 'Ⅱ Duraklat', 'data-lp-transport', 'pause', `${name}: duraklat`);
        const stop = labelledButton('', '■ Durdur', 'data-lp-transport', 'stop', `${name}: durdur`);
        const advance = labelledButton('', '▸| Sonraki', 'data-lp-transport', 'next', `${name}: Sonraki girdiye geç`);
        transport.append(play, pause, stop, advance);

        const level = element('div', 'lp-level');
        const lineBlock = element('div', 'lp-volume-line-control');
        const lineLabel = element('label', '', 'VOLUME');
        lineLabel.htmlFor = `${id}-volume-line`;
        const line = range(`${id}-volume-line`, 'lp-volume-line', 'data-lp-volume-line', `${name} seviye çizgisi`);
        lineBlock.append(lineLabel, line);
        const knobControl = element('div', 'lp-volume-knob knob-control');
        const shell = element('div', 'knob-shell');
        const ring = element('div', 'knob-ring');
        const rotor = element('div', 'knob-rotor');
        ring.setAttribute('aria-hidden', 'true');
        rotor.setAttribute('aria-hidden', 'true');
        const knob = range(`${id}-volume-knob`, 'lp-knob-input', 'data-lp-volume-knob', `${name} seviye düğmesi`);
        shell.append(ring, rotor, knob);
        const volume = element('output', 'lp-volume-value');
        volume.htmlFor = `${line.id} ${knob.id}`;
        knobControl.append(shell, volume);
        const cue = labelledButton('lp-cue', 'CUE', 'data-lp-cue', '', `${name}: Kulaklık monitor seçimi`);
        cue.setAttribute('aria-pressed', 'false');
        level.append(lineBlock, knobControl, cue);

        const details = element('details', 'lp-inputs');
        const summary = element('summary');
        const count = element('span', 'lp-input-count');
        summary.append(element('span', '', 'Girdiler'), count);
        const inputs = element('ol', 'lp-input-list');
        const clear = labelledButton('lp-clear', 'Sırayı temizle', 'data-lp-clear', '', `${name} kartının tüm girdilerini kaldır`);
        details.append(summary, inputs, clear);
        body.append(heading, currentBlock, sourceBlock);
        controls.append(transport, times, level);
        node.append(numberBadge, body, controls, details);

        const card = { id, name, node, queue: [], index: 0, state: 'idle', cue: false, volume: 0,
            refs: { state, source, addInput, current, meta, next, duration, elapsed, count, play, pause, stop, advance, cue, inputs, clear, line, knob, shell, knobControl, volume } };
        cards.set(id, card);
        list.append(node);
        renderSources(card);
        bindVolume(card);
        renderCard(card);
        renderCount();
        return card;
    };
    const renderCount = () => {
        const count = workspace.querySelector('[data-live-player-count]');
        if (count) count.textContent = `${cards.size} oynatıcı`;
        document.dispatchEvent(new CustomEvent('radios:live-player-change', { detail: { players: Array.from(cards.values(), ({ id, name }) => ({ id, name })) } }));
    };

    workspace.addEventListener('change', (event) => {
        if (!(event.target instanceof Element) || !event.target.matches('[data-lp-source]')) return;
        const card = cards.get(event.target.closest('[data-live-player]')?.dataset.livePlayer);
        if (card) card.refs.addInput.disabled = !sources.some((source) => source.id === event.target.value && source.items.length);
    });
    workspace.addEventListener('click', (event) => {
        if (!(event.target instanceof Element)) return;
        const control = event.target.closest('button');
        if (!control || control.disabled) return;
        if (control.matches('[data-add-live-player]')) {
            const card = createCard();
            card.refs.source.focus({ preventScroll: true });
            announce(`${card.name} eklendi.`);
            return;
        }
        const card = cards.get(control.closest('[data-live-player]')?.dataset.livePlayer);
        if (!card) return;
        if (control.matches('[data-lp-remove]')) {
            const focusTarget = (card.node.nextElementSibling || card.node.previousElementSibling)?.querySelector('[data-lp-source]') || workspace.querySelector('[data-add-live-player]');
            cards.delete(card.id);
            card.node.remove();
            focusTarget?.focus({ preventScroll: true });
            renderCount();
            announce(`${card.name} kartı kaldırıldı.`);
            return;
        }
        if (control.matches('[data-lp-add-input]')) {
            const source = sources.find((entry) => entry.id === card.refs.source.value);
            if (source) appendInputs(card, source.items, source.name);
            return;
        }
        if (control.matches('[data-lp-clear]')) {
            card.queue = [];
            card.index = 0;
            card.state = 'idle';
            card.cue = false;
        } else if (control.matches('[data-lp-remove-input]')) {
            const index = card.queue.findIndex((entry) => entry.id === control.dataset.lpRemoveInput);
            if (index < 0) return;
            card.queue.splice(index, 1);
            if (index < card.index) card.index -= 1;
            else if (index === card.index) card.cue = false;
            if (!card.queue.length) { card.index = 0; card.state = 'idle'; card.cue = false; }
            else if (card.index >= card.queue.length) { card.index = card.queue.length; card.state = 'finished'; card.cue = false; }
        } else if (control.matches('[data-lp-cue]')) {
            card.cue = !card.cue;
        } else if (control.matches('[data-lp-transport]')) {
            const action = control.dataset.lpTransport;
            if (action === 'play') {
                if (card.index >= card.queue.length) card.index = 0;
                card.state = 'started';
            } else if (action === 'pause') card.state = 'paused';
            else if (action === 'stop') { card.state = 'idle'; card.index = 0; card.cue = false; }
            else if (action === 'next') {
                card.index += 1;
                card.cue = false;
                if (card.index >= card.queue.length) card.state = 'finished';
            }
        } else return;
        renderCard(card);
        announce(`${card.name}: ${stateLabels[card.state]}${card.cue ? ' · CUE seçili' : ''}.`);
    });

    const acceptsDrop = (event) => Array.from(event.dataTransfer?.types || []).includes(mime);
    workspace.addEventListener('dragover', (event) => {
        if (!(event.target instanceof Element) || !acceptsDrop(event)) return;
        const card = cards.get(event.target.closest('[data-live-player]')?.dataset.livePlayer);
        if (!card) return;
        event.preventDefault();
        event.dataTransfer.dropEffect = 'copy';
        card.node.classList.add('is-drop-target');
    });
    workspace.addEventListener('dragleave', (event) => {
        if (!(event.target instanceof Element)) return;
        const node = event.target.closest('[data-live-player]');
        if (node && (!event.relatedTarget || !node.contains(event.relatedTarget))) node.classList.remove('is-drop-target');
    });
    workspace.addEventListener('drop', (event) => {
        if (!(event.target instanceof Element) || !acceptsDrop(event)) return;
        const card = cards.get(event.target.closest('[data-live-player]')?.dataset.livePlayer);
        if (!card) return;
        event.preventDefault();
        card.node.classList.remove('is-drop-target');
        try {
            const payload = JSON.parse(event.dataTransfer.getData(mime));
            appendInputs(card, payload?.items, text(payload?.playlistName));
        } catch { announce(`${card.name}: Bu girdi aktarımı okunamadı.`); }
    });
    document.addEventListener('dragend', () => cards.forEach((card) => card.node.classList.remove('is-drop-target')));
    ['radios:programme-change', 'radios:playlist-change'].forEach((name) => document.addEventListener(name, refreshSources));

    refreshSources();
    for (let index = 0; index < 6; index += 1) createCard();
    announce('');
})();
