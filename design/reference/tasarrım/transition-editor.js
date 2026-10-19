(() => {
    'use strict';
    const panel = document.querySelector('[data-transition-panel]');
    const dialog = document.querySelector('#transition-editor');
    const form = dialog?.querySelector('[data-transition-form]');
    if (!panel || !dialog || !form) return;
    const storageKey = 'radios.transition-drafts.v1';
    const fields = ['duration', 'cueIn', 'intro', 'out', 'bCueIn', 'fadeIn', 'overlap', 'ducking'];
    const sources = { a: form.querySelector('[data-transition-source="a"]'), b: form.querySelector('[data-transition-source="b"]') };
    const audioFiles = new Map();
    let drafts = {};
    let items = [];
    let selectedProgrammeId = '';
    let nextProgrammeId = '';
    let pair = { a: '', b: '' };
    let audioContext;
    let masterGain;
    let playingNodes = [];
    let playToken = 0;
    let opener;
    let lastPreviewState = '';
    let programmeUpdatePending = false;
    const requests = { a: 0, b: 0 };
    try {
        const parsed = JSON.parse(localStorage.getItem(storageKey) || '{}');
        if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) drafts = parsed;
    } catch (_) { /* Storage can be disabled for a local file. */ }
    const setText = (selector, text, root = document) => {
        const node = root.querySelector(selector);
        if (node) node.textContent = text;
    };
    const field = name => form.elements.namedItem(name);
    const durationSeconds = value => {
        if (typeof value !== 'string' || !/^\d{1,3}:[0-5]\d$/.test(value)) return null;
        const [minutes, seconds] = value.split(':').map(Number);
        return minutes * 60 + seconds;
    };
    const rounded = value => Math.round(value * 10) / 10;
    const secondsLabel = value => Number.isFinite(value) ? `${rounded(value).toLocaleString('tr-TR')} sn` : '—';
    const pairKey = () => JSON.stringify([pair.a, pair.b]);
    const getItem = deck => items.find(item => item.id === pair[deck]);
    const pairMetadata = () => JSON.stringify(['a', 'b'].map(deck => {
        const item = getItem(deck);
        return item ? [item.id, item.title, item.artist || '', item.album || '', item.duration || ''] : null;
    }));
    const fileFor = deck => audioFiles.get(pair[deck]);
    const setStatus = (message, error = false) => {
        const node = form.querySelector('[data-transition-editor-status]');
        node.textContent = message;
        node.classList.toggle('is-error', error);
    };
    const readValues = () => {
        const values = Object.fromEntries(fields.map(name => [name, field(name).value === '' ? null : Number(field(name).value)]));
        values.locked = field('locked').checked;
        return values;
    };
    const validation = (values = readValues(), mark = false) => {
        if (mark) fields.forEach(name => field(name).setCustomValidity(''));
        const reject = (message, name) => {
            if (mark && name) field(name).setCustomValidity(message);
            return message;
        };
        if (!getItem('a') || !getItem('b') || pair.a === pair.b) return reject('A ve B farklı öğeler olmalı.');
        if (!Number.isFinite(values.duration) || values.duration <= 0 || values.duration > 86400) return reject('A toplam süresi gerekli.', 'duration');
        if (!Number.isFinite(values.cueIn) || values.cueIn < 0 || values.cueIn >= values.duration) return reject('A cue başlangıcı, sıfır ile dosya süresi arasında olmalı.', 'cueIn');
        if (!Number.isFinite(values.out) || values.out <= values.cueIn || values.out > values.duration) return reject('Çıkış noktası cue başlangıcından sonra ve A toplam süresi içinde olmalı.', 'out');
        if (values.intro !== null && (!Number.isFinite(values.intro) || values.intro < values.cueIn || values.intro > values.out)) return reject('Vokal girişi, A cue başlangıcı ile çıkış noktası arasında olmalı.', 'intro');
        if (!Number.isFinite(values.bCueIn) || values.bCueIn < 0 || values.bCueIn > 86400) return reject('B cue başlangıcını sıfır veya daha büyük gir.', 'bCueIn');
        if (!Number.isFinite(values.overlap) || values.overlap < 0 || values.overlap > 30 || values.overlap > values.out - values.cueIn) return reject('Bindirme, A oynatma süresini aşamaz; en fazla 30 saniye olabilir.', 'overlap');
        if (!Number.isFinite(values.fadeIn) || values.fadeIn < 0 || values.fadeIn > 30) return reject('Giriş fade süresi 0–30 saniye arasında olmalı.', 'fadeIn');
        if (!Number.isFinite(values.ducking) || values.ducking < -30 || values.ducking > 0) return reject('A kısma değeri −30 ile 0 dB arasında olmalı.', 'ducking');
        const a = fileFor('a'), b = fileFor('b');
        if (a && values.out > a.buffer.duration + 0.05) return reject('Çıkış noktası yüklenen A dosyasının süresini aşamaz.', 'out');
        const bDuration = b?.buffer.duration ?? durationSeconds(getItem('b')?.duration);
        if (bDuration !== null && Number.isFinite(bDuration)) {
            if (values.bCueIn >= bDuration) return reject('B cue başlangıcı B dosyasının süresinden önce olmalı.', 'bCueIn');
            if (Math.max(values.overlap, values.fadeIn) > bDuration - values.bCueIn) return reject('Bindirme ve giriş fade’i, B dosyasında kalan süreyi aşamaz.', 'fadeIn');
        }
        return '';
    };
    const canPreview = () => Boolean(fileFor('a') && fileFor('b') && !validation());
    const publishPreviewState = () => {
        const detail = { available: canPreview(), playing: playingNodes.length > 0, label: 'Tarayıcı ses çıkışı' };
        const serialized = JSON.stringify(detail);
        if (lastPreviewState !== serialized) {
            lastPreviewState = serialized;
            document.dispatchEvent(new CustomEvent('radios:preview-source-change', { detail }));
        }
    };
    const syncPreviewControls = () => {
        const available = canPreview();
        form.querySelector('[data-transition-preview]').disabled = !available;
        form.querySelector('[data-transition-stop]').disabled = !playingNodes.length;
        form.querySelector('[data-transition-preview]').textContent = playingNodes.length ? 'Yeniden dinle' : 'Ön dinle';
        publishPreviewState();
    };
    const stop = (announce = false) => {
        ++playToken;
        const nodes = playingNodes;
        playingNodes = [];
        for (const node of nodes) {
            node.source.onended = null;
            try { node.source.stop(); } catch (_) { /* An already-ended source needs no action. */ }
            node.source.disconnect();
            node.gain.disconnect();
        }
        if (masterGain) { masterGain.disconnect(); masterGain = undefined; }
        syncPreviewControls();
        if (announce && nodes.length) setStatus('Ön dinleme durduruldu.');
    };
    const ensureContext = () => {
        if (!audioContext || audioContext.state === 'closed') {
            const Context = window.AudioContext || window.webkitAudioContext;
            if (!Context) throw new Error('Bu tarayıcı yerel ses ön dinlemesini desteklemiyor.');
            audioContext = new Context();
        }
        return audioContext;
    };
    const currentLevel = () => {
        const main = document.querySelector('#headphone-gain');
        const value = Number(main?.value ?? form.querySelector('#transition-preview-level').value);
        return Number.isFinite(value) ? Math.min(6, Math.max(-60, value)) : -12;
    };
    const syncLevel = () => {
        const value = currentLevel();
        const range = form.querySelector('#transition-preview-level');
        range.min = '-60'; range.max = '6'; range.value = String(value);
        setText('[data-transition-level-output]', `${value < 0 ? '−' : value > 0 ? '+' : ''}${Math.abs(value).toLocaleString('tr-TR')} dB`, form);
        if (masterGain && audioContext) masterGain.gain.setTargetAtTime(Math.pow(10, value / 20), audioContext.currentTime, 0.03);
    };
    const preview = async () => {
        const problem = validation();
        if (problem || !fileFor('a') || !fileFor('b')) {
            setStatus(problem || 'A ve B ses dosyası yok.', true);
            syncPreviewControls();
            return false;
        }
        stop();
        const token = playToken;
        try {
            const context = ensureContext();
            await context.resume();
            if (token !== playToken) return false;
            const values = readValues();
            const a = fileFor('a').buffer, b = fileFor('b').buffer;
            const offsetA = Math.max(values.cueIn, values.out - values.overlap - 4);
            const lengthA = values.out - offsetA;
            const delayB = lengthA - values.overlap;
            const lengthB = Math.min(b.duration - values.bCueIn, Math.max(values.overlap, values.fadeIn) + 4);
            const start = context.currentTime + 0.06;
            masterGain = context.createGain();
            masterGain.gain.value = Math.pow(10, currentLevel() / 20);
            masterGain.connect(context.destination);
            const makeNode = buffer => {
                const source = context.createBufferSource();
                const gain = context.createGain();
                source.buffer = buffer; source.connect(gain); gain.connect(masterGain);
                const node = { source, gain };
                playingNodes.push(node);
                source.onended = () => {
                    if (token !== playToken) return;
                    playingNodes = playingNodes.filter(candidate => candidate !== node);
                    source.disconnect(); gain.disconnect();
                    if (!playingNodes.length) { masterGain?.disconnect(); masterGain = undefined; setStatus('Ön dinleme tamamlandı. Yayın akışı değişmedi.'); }
                    syncPreviewControls();
                };
                return node;
            };
            const nodeA = makeNode(a), nodeB = makeNode(b);
            nodeA.gain.gain.setValueAtTime(1, start);
            if (values.overlap > 0) {
                const duck = Math.pow(10, values.ducking / 20);
                nodeA.gain.gain.setValueAtTime(1, start + delayB);
                nodeA.gain.gain.linearRampToValueAtTime(duck, start + delayB + Math.min(0.1, values.overlap / 2));
                nodeA.gain.gain.linearRampToValueAtTime(0, start + lengthA);
            }
            nodeB.gain.gain.setValueAtTime(values.fadeIn > 0 ? 0 : 1, start + delayB);
            if (values.fadeIn > 0) nodeB.gain.gain.linearRampToValueAtTime(1, start + delayB + values.fadeIn);
            // Preview end gets a brief safety fade, independent of the stored transition.
            const bEnd = start + delayB + lengthB;
            nodeB.gain.gain.setValueAtTime(1, Math.max(start + delayB + values.fadeIn, bEnd - 0.04));
            nodeB.gain.gain.linearRampToValueAtTime(0, bEnd);
            nodeA.source.start(start, offsetA, lengthA);
            nodeB.source.start(start + delayB, values.bCueIn, lengthB);
            setStatus('Ön dinleme çalıyor.');
            syncPreviewControls();
            return true;
        } catch (error) {
            stop();
            setStatus(`Ön dinleme başlatılamadı: ${error.message || 'Ses aygıtını kontrol et.'}`, true);
            return false;
        }
    };
    const drawWave = deck => {
        const canvas = form.querySelector(`[data-transition-wave="${deck}"]`);
        const drawing = canvas.getContext('2d');
        const width = canvas.width, height = canvas.height;
        drawing.clearRect(0, 0, width, height);
        drawing.fillStyle = '#030303'; drawing.fillRect(0, 0, width, height);
        drawing.strokeStyle = '#333333'; drawing.lineWidth = 1;
        drawing.beginPath(); drawing.moveTo(0, height / 2); drawing.lineTo(width, height / 2); drawing.stroke();
        const loaded = fileFor(deck);
        canvas.setAttribute('aria-label', `${deck.toUpperCase()} öğesinin ${loaded ? 'yüklenen dosyadan hesaplanan gerçek dalga biçimi' : 'dalga biçimi; ses dosyası bekleniyor'}`);
        if (!loaded) {
            return;
        }
        const data = loaded.buffer.getChannelData(0);
        const stride = Math.max(1, Math.floor(data.length / width));
        drawing.strokeStyle = deck === 'a' ? '#cccccc' : '#888888';
        drawing.beginPath();
        for (let x = 0; x < width; x++) {
            let min = 0, max = 0;
            const first = Math.floor(x * data.length / width), end = Math.min(data.length, first + stride);
            // Peak sampling is bounded for very long files and remains derived from the real audio.
            const increment = Math.max(1, Math.floor(stride / 300));
            for (let index = first; index < end; index += increment) { min = Math.min(min, data[index]); max = Math.max(max, data[index]); }
            drawing.moveTo(x + .5, height / 2 - max * (height / 2 - 5));
            drawing.lineTo(x + .5, height / 2 - min * (height / 2 - 5));
        }
        drawing.stroke();
        const values = readValues();
        const marks = deck === 'a' ? [[values.cueIn, '#64dda6'], [values.intro, '#eccd7a'], [values.out, '#f7a284']] : [[values.bCueIn, '#64dda6']];
        for (const [time, color] of marks) {
            if (!Number.isFinite(time) || time < 0 || time > loaded.buffer.duration) continue;
            const x = time / loaded.buffer.duration * width;
            drawing.strokeStyle = color; drawing.beginPath(); drawing.moveTo(x, 0); drawing.lineTo(x, height); drawing.stroke();
        }
    };
    const updateFiles = () => {
        field('duration').readOnly = Boolean(fileFor('a'));
        for (const deck of ['a', 'b']) {
            const loaded = fileFor(deck);
            setText(`[data-transition-file-info="${deck}"]`, loaded ? `${loaded.name} · ${secondsLabel(loaded.buffer.duration)}` : 'Dosya yüklenmedi', form);
            form.querySelector(`[data-clear-transition-file="${deck}"]`).disabled = !loaded;
            drawWave(deck);
        }
    };
    const updateRanges = () => {
        setText('[data-transition-overlap-output]', secondsLabel(Number(field('overlap').value)), form);
        const duck = Number(field('ducking').value);
        setText('[data-transition-ducking-output]', `${duck < 0 ? '−' : ''}${Math.abs(duck)} dB`, form);
    };
    const renderPanel = () => {
        const a = getItem('a'), b = getItem('b');
        setText('[data-transition-context]', a && b ? `${a.title} → ${b.title}` : '—', panel);
        setText('[data-transition-a]', a?.title || 'Öğe seçilmedi', panel);
        setText('[data-transition-b]', b?.title || 'Sonraki öğe seçilmedi', panel);
        for (const deck of ['a', 'b']) {
            const item = getItem(deck);
            setText(`[data-transition-${deck}-meta]`, item ? [item.artist, item.album, item.duration].filter(Boolean).join(' · ') || '—' : deck === 'a' ? 'Çıkış öğesi' : 'Giriş öğesi', panel);
        }
        const saved = drafts[pairKey()];
        for (const name of ['cueIn', 'intro', 'out', 'overlap']) setText(`[data-transition-summary="${name}"]`, saved ? secondsLabel(saved[name]) : '—', panel);
        setText('[data-transition-mini-overlap]', saved ? `${secondsLabel(saved.overlap)} bindirme${saved.locked ? ' · Kilitli' : ''}` : '—', panel);
        setText('[data-transition-panel-status]', '', panel);
    };
    const loadPair = (stopAudio = true) => {
        if (stopAudio) stop();
        programmeUpdatePending = false;
        fields.forEach(name => field(name).setCustomValidity(''));
        const saved = drafts[pairKey()];
        const total = fileFor('a')?.buffer.duration ?? durationSeconds(getItem('a')?.duration);
        const defaults = { duration: total === null || total === undefined ? '' : rounded(total), cueIn: 0, intro: '', out: total === null || total === undefined ? '' : rounded(total), bCueIn: 0, fadeIn: 2, overlap: 2, ducking: 0 };
        for (const name of fields) {
            const value = saved?.[name] ?? defaults[name];
            field(name).value = Number.isFinite(value) ? String(value) : '';
        }
        field('locked').checked = Boolean(saved?.locked);
        // Loaded audio, then current programme metadata, defines the known source duration.
        // A stored manual duration is only a fallback while neither source is available.
        if (Number.isFinite(total)) field('duration').value = String(rounded(total));
        for (const deck of ['a', 'b']) {
            sources[deck].value = pair[deck];
            form.querySelector(`[data-transition-file="${deck}"]`).value = '';
        }
        updateRanges(); updateFiles(); renderPanel(); syncLevel(); syncPreviewControls();
    };
    const populateSources = () => {
        for (const deck of ['a', 'b']) {
            const fragment = document.createDocumentFragment();
            const empty = document.createElement('option'); empty.value = ''; empty.textContent = 'Öğe seç'; fragment.append(empty);
            for (const item of items) {
                const option = document.createElement('option'); option.value = item.id;
                option.textContent = `${item.title}${item.artist ? ` · ${item.artist}` : ''}`;
                fragment.append(option);
            }
            sources[deck].replaceChildren(fragment); sources[deck].value = pair[deck];
        }
    };
    const acceptSnapshot = snapshot => {
        if (!snapshot?.programme || !Array.isArray(snapshot.programme.items)) return;
        const previousKey = pairKey();
        const previousMetadata = pairMetadata();
        items = (snapshot.playbackItems || snapshot.programme.items).filter(item => item && typeof item.id === 'string' && typeof item.title === 'string');
        const selected = snapshot.selectedItem?.id || snapshot.selectedId || items[0]?.id || '';
        const index = items.findIndex(item => item.id === selected);
        const next = items[index >= 0 ? index + 1 : 1]?.id || items.find(item => item.id !== selected)?.id || '';
        const changed = selected !== selectedProgrammeId || (pair.a === selected && next !== nextProgrammeId);
        selectedProgrammeId = selected; nextProgrammeId = next;
        if (changed || !getItem('a') || !getItem('b')) {
            pair.a = items[index >= 0 ? index : 0]?.id || '';
            pair.b = items[index >= 0 ? index + 1 : 1]?.id || items.find(item => item.id !== pair.a)?.id || '';
        }
        populateSources();
        if (previousKey !== pairKey() || !form.dataset.ready) {
            form.dataset.ready = 'true'; ++requests.a; ++requests.b; loadPair();
            if (dialog.open) setStatus('A/B çifti değişti.');
        } else if (previousMetadata !== pairMetadata()) {
            if (dialog.open) {
                programmeUpdatePending = true;
                renderPanel(); syncPreviewControls();
                setStatus('Program öğesinin süresi veya bilgileri değişti. Düzenlediğin işaretler korundu; editörü kapatınca güncel program bilgileri yeniden yüklenir.');
            } else {
                // Same audio sources can keep playing through harmless metadata refreshes.
                loadPair(false);
                if (playingNodes.length && !canPreview()) stop();
            }
        } else { renderPanel(); syncPreviewControls(); }
    };
    for (const deck of ['a', 'b']) {
        sources[deck].addEventListener('change', () => {
            ++requests.a; ++requests.b;
            pair[deck] = sources[deck].value;
            if (pair.a && pair.a === pair.b) pair[deck === 'a' ? 'b' : 'a'] = items.find(item => item.id !== pair[deck])?.id || '';
            loadPair(); setStatus('');
        });
        form.querySelector(`[data-transition-file="${deck}"]`).addEventListener('change', async event => {
            const file = event.target.files?.[0];
            if (!file) return;
            const id = pair[deck];
            if (!id) { event.target.value = ''; setStatus('Program öğesi seçili değil.', true); return; }
            if (file.size > 100 * 1024 * 1024) { event.target.value = ''; setStatus('Dosya 100 MB sınırını aşıyor.', true); return; }
            const request = ++requests[deck];
            stop(); setStatus(`${deck.toUpperCase()} dosyası çözümleniyor…`);
            try {
                const buffer = await ensureContext().decodeAudioData(await file.arrayBuffer());
                if (request !== requests[deck] || pair[deck] !== id) return;
                if (!Number.isFinite(buffer.duration) || buffer.duration < 0.1 || buffer.duration > 86400) throw new Error('Dosya süresi desteklenen aralığın dışında.');
                audioFiles.set(id, { buffer, name: file.name });
                if (deck === 'a') {
                    field('duration').value = String(rounded(buffer.duration));
                    const previousOut = field('out').value === '' ? NaN : Number(field('out').value);
                    if (!Number.isFinite(previousOut) || previousOut > buffer.duration) field('out').value = String(rounded(buffer.duration));
                    if (Number(field('cueIn').value) >= buffer.duration) field('cueIn').value = '0';
                }
                fields.forEach(name => field(name).setCustomValidity(''));
                updateFiles(); syncPreviewControls();
                setStatus(`${deck.toUpperCase()} dosyası yüklendi.`);
            } catch (error) {
                if (request !== requests[deck]) return;
                event.target.value = '';
                setStatus(`Ses dosyası açılamadı${error.message ? `: ${error.message}` : '.'}`, true);
                syncPreviewControls();
            }
        });
        form.querySelector(`[data-clear-transition-file="${deck}"]`).addEventListener('click', () => {
            ++requests[deck]; stop(); audioFiles.delete(pair[deck]);
            form.querySelector(`[data-transition-file="${deck}"]`).value = '';
            updateFiles(); syncPreviewControls(); setStatus(`${deck.toUpperCase()} dosyası kaldırıldı.`);
        });
    }
    for (const name of fields) field(name).addEventListener('input', () => {
        stop(); fields.forEach(key => field(key).setCustomValidity(''));
        updateRanges(); drawWave('a'); drawWave('b'); syncPreviewControls();
    });
    const open = button => {
        opener = button;
        const snapshot = window.RadioSProgramme?.getSnapshot?.();
        if (snapshot) acceptSnapshot(snapshot);
        if (programmeUpdatePending && !dialog.open) loadPair(false);
        if (!dialog.open) dialog.showModal();
        updateFiles(); syncLevel(); syncPreviewControls();
        setStatus('');
    };
    document.addEventListener('click', event => {
        const openButton = event.target.closest('[data-open-transition-editor]');
        if (openButton) open(openButton);
        if (event.target.closest('[data-close-transition-editor]')) { stop(); dialog.close(); }
    });
    dialog.addEventListener('cancel', () => stop());
    dialog.addEventListener('close', () => {
        stop();
        // Cancelling the dialog restores the stored markers while keeping loaded audio available.
        // A row re-render can replace the opener; return focus to the same transition line.
        const target = opener?.isConnected ? opener : document.querySelector(`[data-programme-transition="${CSS.escape(pair.a)}"]`) || opener;
        loadPair(); target?.focus({ preventScroll: true });
    });
    form.addEventListener('submit', event => {
        event.preventDefault();
        const values = readValues();
        const problem = validation(values, true);
        if (problem) { setStatus(problem, true); form.reportValidity(); return; }
        stop();
        drafts[pairKey()] = { ...values };
        let stored = true;
        try { localStorage.setItem(storageKey, JSON.stringify(drafts)); } catch (_) { stored = false; }
        document.dispatchEvent(new CustomEvent('radios:transition-saved', { detail: { fromId: pair.a, toId: pair.b, ...values } }));
        renderPanel();
        setStatus(stored ? 'Kaydedildi.' : 'Kalıcı kayıt yapılamadı.');
        dialog.close();
    });
    form.querySelector('[data-transition-preview]').addEventListener('click', preview);
    form.querySelector('[data-transition-stop]').addEventListener('click', () => stop(true));
    form.querySelector('#transition-preview-level').addEventListener('input', event => {
        const main = document.querySelector('#headphone-gain');
        if (main) { main.value = event.target.value; main.dispatchEvent(new Event('input', { bubbles: true })); }
        syncLevel();
    });
    document.querySelector('#headphone-gain')?.addEventListener('input', syncLevel);
    document.addEventListener('radios:programme-change', event => acceptSnapshot(event.detail));
    for (const eventName of ['radios:profile-change', 'radios:workspace-profile-change']) document.addEventListener(eventName, () => { stop(); if (dialog.open) dialog.close(); });
    document.addEventListener('visibilitychange', () => { if (document.hidden) stop(); });
    window.addEventListener('pagehide', () => { stop(); audioContext?.close().catch(() => {}); });
    window.RadioSTransition = {
        getSnapshot: () => ({ fromId: pair.a, toId: pair.b, ...readValues(), saved: Boolean(drafts[pairKey()]), available: canPreview(), playing: playingNodes.length > 0 }),
        canPreview, preview, stop: () => stop(true),
    };
    const initial = window.RadioSProgramme?.getSnapshot?.();
    if (initial) acceptSnapshot(initial);
    else { populateSources(); loadPair(); }
})();
