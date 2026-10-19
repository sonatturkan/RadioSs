(() => {
    const read = (key, fallback) => { try { return JSON.parse(localStorage.getItem(key)) || fallback; } catch { return fallback; } };
    const write = (key, value) => { try { localStorage.setItem(key, JSON.stringify(value)); return true; } catch { return false; } };
    const text = (value, max = 160) => typeof value === 'string' ? value.trim().slice(0, max) : '';
    const node = (tag, className, value) => { const result = document.createElement(tag); if (className) result.className = className; if (value !== undefined) result.textContent = value; return result; };
    const status = (selector, message) => { const target = document.querySelector(selector); if (target) target.textContent = message; };
    const snapshot = () => window.RadioSProgramme?.getSnapshot();
    const cleanEmergency = (value) => ({
        strategy: ['playlist', 'next', 'continue'].includes(value?.strategy) ? value.strategy : 'playlist',
        name: text(value?.name, 100),
    });
    const cleanItem = (item) => {
        if (!item || typeof item !== 'object' || !text(item.title)) return null;
        const type = ['music', 'announcement', 'advert', 'jingle'].includes(item.type) ? item.type : 'music';
        const duration = text(item.duration, 6);
        return { type, title: text(item.title), artist: text(item.artist, 120), album: text(item.album, 120), duration: /^[0-9]{1,3}:[0-5][0-9]$/.test(duration) ? duration : '', version: text(item.version, 80), readiness: ['pending', 'ready', 'missing', 'approval', 'invalid', 'locked'].includes(item.readiness) ? item.readiness : 'pending', primaryState: text(item.primaryState, 20), backupState: text(item.backupState, 20), notes: text(item.notes, 300) };
    };
    const stored = read('radios.operations.v1', {});
    let nextId = 0;
    const queue = Array.isArray(stored.queue) ? stored.queue.map(cleanItem).filter(Boolean).map((item) => ({ ...item, id: `temporary-${++nextId}` })) : [];
    let selectedId = queue[0]?.id || null;
    const audit = Array.isArray(stored.audit) ? stored.audit.filter((entry) => entry && typeof entry.message === 'string' && typeof entry.time === 'string').slice(0, 100) : [];
    const settings = { mode: stored.settings?.mode === 'hold' ? 'hold' : 'auto', source: ['microphone', 'remote', 'bed'].includes(stored.settings?.source) ? stored.settings.source : 'microphone', bed: Number.isFinite(stored.settings?.bed) ? Math.max(-36, Math.min(0, stored.settings.bed)) : -18, ducking: stored.settings?.ducking !== false, emergency: cleanEmergency(stored.settings?.emergency) };
    const persist = () => write('radios.operations.v1', { queue, audit: audit.slice(0, 100), settings });
    const renderAudit = () => {
        const target = document.querySelector('[data-audit-list]');
        if (!target) return;
        const fragment = document.createDocumentFragment();
        for (const entry of audit.slice(0, 100)) {
            const li = node('li');
            const date = new Date(entry.time);
            const time = node('time', '', Number.isNaN(date.getTime()) ? '—' : date.toLocaleString('tr-TR', { timeZone: 'UTC' }));
            time.dateTime = entry.time;
            li.append(time, node('span', '', entry.message));
            fragment.append(li);
        }
        if (!audit.length) fragment.append(node('li', 'ops-queue-empty', 'Kayıt yok'));
        target.replaceChildren(fragment);
    };
    const record = (message) => { audit.unshift({ time: new Date().toISOString(), message: text(message, 300), scope: 'local-draft' }); audit.splice(100); persist(); renderAudit(); };
    const saveEmergency = (values) => {
        settings.emergency = cleanEmergency(values);
        record('Acil devralma davranış taslağı kaydedildi.');
        document.dispatchEvent(new CustomEvent('radios:emergency-draft-change', { detail: { ...settings.emergency } }));
    };
    window.RadioSOperations = Object.freeze({ emergencySnapshot: () => ({ ...settings.emergency }) });
    document.addEventListener('radios:emergency-draft-request', (event) => saveEmergency(event.detail));
    const selectedQueueItem = () => queue.find((item) => item.id === selectedId);
    const renderQueue = () => {
        const target = document.querySelector('[data-temporary-queue-list]');
        if (!target) return;
        const focusedId = target.contains(document.activeElement) ? document.activeElement.dataset.queueSelect : null;
        const fragment = document.createDocumentFragment();
        queue.forEach((item, index) => {
            const li = node('li');
            const select = node('button');
            select.type = 'button'; select.dataset.queueSelect = item.id;
            select.setAttribute('aria-pressed', String(item.id === selectedId));
            const info = node('span');
            info.append(node('strong', '', item.title));
            select.setAttribute('aria-label', `${index + 1}. ${item.title}`);
            const metadata = [item.artist, item.album].filter(Boolean).join(' · ');
            if (metadata) info.append(node('small', '', metadata));
            select.append(info, node('span', 'ops-queue-duration', item.duration || '—'));
            li.append(select); fragment.append(li);
        });
        if (!queue.length) fragment.append(node('li', 'ops-queue-empty', 'Sıra boş'));
        target.replaceChildren(fragment);
        if (focusedId) target.querySelector(`[data-queue-select="${focusedId}"]`)?.focus({ preventScroll: true });
        status('[data-queue-count]', `${queue.length} öğe`);
        const index = queue.findIndex((item) => item.id === selectedId);
        document.querySelectorAll('[data-queue-action]').forEach((button) => { button.disabled = index < 0 || (button.dataset.queueAction === 'up' && index === 0) || (button.dataset.queueAction === 'down' && index === queue.length - 1); });
        const transfer = document.querySelector('[data-queue-transfer]'); if (transfer) transfer.disabled = !queue.length;
        const source = document.querySelector('[data-queue-from-selected]'); if (source) source.disabled = !snapshot()?.selectedItem;
    };
    const addQueue = (items) => {
        const added = (Array.isArray(items) ? items : []).map(cleanItem).filter(Boolean).map((item) => ({ ...item, id: `temporary-${++nextId}` }));
        if (!added.length) return;
        queue.push(...added); selectedId = added[0].id; persist(); renderQueue(); record(`${added.length} öğe geçici sıraya eklendi.`);
        status('[data-queue-status]', `${added.length} öğe eklendi.`);
    };
    const show = (id) => { const dialog = document.getElementById(id); if (dialog && !dialog.open) dialog.showModal(); };
    const close = (dialog) => { if (dialog?.open) dialog.close(); };
    const fields = (form, values) => { form.reset(); for (const [name, value] of Object.entries(values)) { const field = form.elements.namedItem(name); if (field && 'value' in field) field.value = value ?? ''; } };
    let queueEditorId = null;
    const editQueue = (isNew = false) => {
        const form = document.querySelector('[data-queue-form]'); if (!form) return;
        const item = isNew ? null : selectedQueueItem(); if (!isNew && !item) return;
        queueEditorId = item?.id || null;
        fields(form, item || { title: '', type: 'music', artist: '', album: '', duration: '' });
        show('temporary-queue-editor');
    };
    const download = (value, name) => {
        const url = URL.createObjectURL(new Blob([JSON.stringify(value, null, 2)], { type: 'application/json' }));
        const link = document.createElement('a'); link.href = url; link.download = name; document.body.append(link); link.click(); link.remove(); setTimeout(() => URL.revokeObjectURL(url), 1000);
    };
    let lastRevision = snapshot()?.programme?.revision;
    document.addEventListener('radios:programme-change', (event) => {
        renderQueue();
        const revision = event.detail?.programme?.revision;
        if (lastRevision !== undefined && revision !== lastRevision) record(`Program taslağı değişti: revizyon ${revision}.`);
        lastRevision = revision;
    });
    document.addEventListener('radios:transition-saved', () => record('Geçiş taslağı kaydedildi.'));

    const temporaryTarget = document.getElementById('temporary-queue');
    const mime = 'application/x-radios-programme-items';
    for (const name of ['dragenter', 'dragover']) temporaryTarget?.addEventListener(name, (event) => {
        if (!Array.from(event.dataTransfer?.types || []).includes(mime)) return;
        event.preventDefault(); event.dataTransfer.dropEffect = 'copy'; temporaryTarget.classList.add('is-drop-target');
    });
    temporaryTarget?.addEventListener('dragleave', (event) => { if (!temporaryTarget.contains(event.relatedTarget)) temporaryTarget.classList.remove('is-drop-target'); });
    temporaryTarget?.addEventListener('drop', (event) => {
        temporaryTarget.classList.remove('is-drop-target');
        if (!Array.from(event.dataTransfer?.types || []).includes(mime)) return;
        event.preventDefault();
        try { addQueue(JSON.parse(event.dataTransfer.getData(mime)).items); } catch { status('[data-queue-status]', 'Aktarım okunamadı.'); }
    });
    document.addEventListener('dragend', () => temporaryTarget?.classList.remove('is-drop-target'));

    const mode = document.getElementById('operations-mode-request');
    if (mode) { mode.value = settings.mode; mode.addEventListener('change', () => { settings.mode = mode.value; persist(); record(`Devam tercihi taslağı: ${mode.selectedOptions[0].textContent}.`); }); }
    const speechSource = document.getElementById('speech-source');
    const speechBed = document.getElementById('speech-bed');
    const speechDucking = document.getElementById('speech-ducking');
    if (speechSource) { speechSource.value = settings.source; speechSource.addEventListener('change', () => { settings.source = speechSource.value; persist(); }); }
    const renderBed = () => status('[data-speech-bed-value]', `${settings.bed} dB`);
    if (speechBed) { speechBed.value = settings.bed; speechBed.addEventListener('input', () => { settings.bed = Number(speechBed.value); persist(); renderBed(); }); }
    if (speechDucking) { speechDucking.checked = settings.ducking; speechDucking.addEventListener('change', () => { settings.ducking = speechDucking.checked; persist(); }); }
    let rehearsalStart = 0; let rehearsalElapsed = 0; let rehearsalTimer = null;
    const renderRehearsal = () => { const seconds = Math.floor((rehearsalElapsed + (rehearsalTimer ? performance.now() - rehearsalStart : 0)) / 1000); status('[data-rehearsal-time]', `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`); };
    const pauseRehearsal = () => { if (!rehearsalTimer) return; rehearsalElapsed += performance.now() - rehearsalStart; clearInterval(rehearsalTimer); rehearsalTimer = null; status('[data-rehearsal-toggle]', 'Prova sürdür'); renderRehearsal(); };

    const assignments = new Map();
    const padMetadata = read('radios.pad-assignments.v1', {});
    const selectedBanks = { sampler: stored.settings?.selectedBanks?.sampler === 'programme' ? 'programme' : 'general', jingle: stored.settings?.selectedBanks?.jingle === 'programme' ? 'programme' : 'general' };
    settings.selectedBanks = selectedBanks;
    let soloBank = null;
    const mutedBanks = new Set();
    const bankKey = (bank, slot, group = selectedBanks[bank]) => group === 'programme' ? `${bank}:programme:${slot}` : `${bank}:${slot}`;
    const padButton = (bank, slot) => { const page = slot <= 4 ? 1 : 2; return document.querySelector(`[data-pad-bank="${bank}"] [data-pad-page="${page}"]`)?.querySelectorAll('button')[((slot - 1) % 4)]; };
    const setBankStatus = (bank, message) => status(`[data-pad-bank="${bank}"] .bank-context`, `${selectedBanks[bank] === 'programme' ? 'Program bankası · ' : ''}${message}`);
    const renderPad = (bank, slot) => {
        const key = bankKey(bank, slot); const item = assignments.get(key); const meta = padMetadata[key]; const button = padButton(bank, slot); if (!button) return;
        button.disabled = !item; button.setAttribute('aria-pressed', String(item?.playing === true)); button.classList.toggle('is-previewing', item?.playing === true);
        button.querySelector('small').textContent = item?.playing ? 'Ön dinleme' : item?.title || meta?.title || 'Boş';
        const oldDuration = button.querySelector('.pad-duration');
        if (Number.isFinite(item?.duration) && item.duration > 0) {
            const seconds = Math.ceil(item.duration);
            const duration = oldDuration || node('small', 'pad-duration');
            duration.textContent = `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`;
            if (!oldDuration) button.append(duration);
        } else oldDuration?.remove();
        button.title = item?.title || meta?.title || '';
        button.setAttribute('aria-label', `${bank === 'sampler' ? 'Sampler' : 'Jingle'} ${slot}, ${item?.title || meta?.title || 'atanmadı'}${item?.playing ? ', ön dinleniyor' : ''}`);
        document.querySelectorAll(`[data-pad-bank="${bank}"] [data-pad-stop], [data-pad-bank="${bank}"] [data-pad-mute], [data-pad-bank="${bank}"] [data-pad-solo]`).forEach((control) => { control.disabled = !Array.from({ length: 8 }, (_, index) => assignments.has(bankKey(bank, index + 1))).some(Boolean); });
    };
    const gainValue = () => Math.max(0, Math.min(1, Math.pow(10, Number(document.getElementById('headphone-gain')?.value ?? -12) / 20)));
    const updatePadGains = () => assignments.forEach((item) => { item.audio.volume = gainValue(); item.audio.muted = mutedBanks.has(item.bank) || (soloBank !== null && item.bank !== soloBank); });
    const stopPad = (item) => { if (!item) return; item.audio.pause(); item.audio.currentTime = 0; item.playing = false; renderPad(item.bank, item.slot); };
    const stopPads = (bank) => { assignments.forEach((item) => { if (!bank || item.bank === bank) stopPad(item); }); if (bank) setBankStatus(bank, 'Durduruldu'); };
    const playPad = async (item) => {
        if (!item) return;
        window.RadioSTransition?.stop();
        item.audio.loop = item.trigger === 'loop'; updatePadGains();
        try { item.audio.currentTime = 0; await item.audio.play(); }
        catch { item.playing = false; renderPad(item.bank, item.slot); setBankStatus(item.bank, 'Bu dosya ön dinlenemedi'); }
    };
    const openAssignment = (bank) => { const form = document.querySelector('[data-pad-assignment-form]'); if (!form) return; const page = document.querySelector(`[data-pad-bank="${bank}"]`)?.dataset.activePage || '1'; fields(form, { bank, slot: page === '2' ? '5' : '1', title: '', trigger: 'once' }); status('[data-pad-assignment-status]', ''); show('pad-assignment-dialog'); };
    for (const bank of document.querySelectorAll('[data-pad-bank]')) {
        const bankName = bank.dataset.padBank;
        const tools = node('div', 'pad-bank-tools');
        const label = node('label', 'pad-bank-selector', 'Banka');
        const select = node('select'); select.dataset.padBankSelect = bankName;
        select.setAttribute('aria-label', `${bankName === 'sampler' ? 'Sampler' : 'Jingle'} bankası`);
        for (const [value, title] of [['general', 'Genel banka'], ['programme', 'Program bankası']]) { const option = node('option', '', title); option.value = value; select.append(option); }
        select.value = selectedBanks[bankName]; label.append(select); tools.append(label);
        select.addEventListener('change', () => {
            stopPads(bankName); selectedBanks[bankName] = select.value; persist();
            for (let slot = 1; slot <= 8; slot += 1) renderPad(bankName, slot);
            setBankStatus(bankName, Array.from({ length: 8 }, (_, index) => assignments.has(bankKey(bankName, index + 1))).some(Boolean) ? 'Hazır' : '—');
            record(`${bankName}: ${select.selectedOptions[0].textContent} seçildi.`);
        });
        for (const [key, label] of [['assign', 'Ses ata'], ['stop', 'Durdur'], ['mute', 'Mute'], ['solo', 'Solo']]) {
            const control = node('button', '', label); control.type = 'button'; control.dataset[`pad${key[0].toUpperCase()}${key.slice(1)}`] = bankName;
            if (['mute', 'solo'].includes(key)) control.setAttribute('aria-pressed', 'false');
            if (key !== 'assign') control.disabled = true;
            tools.append(control);
        }
        bank.append(tools);
        for (let slot = 1; slot <= 8; slot += 1) {
            const control = padButton(bankName, slot); if (!control) continue;
            renderPad(bankName, slot);
            control.addEventListener('click', () => {
                const item = assignments.get(bankKey(bankName, slot)); if (!item || item.trigger === 'hold') return;
                if (item.playing && ['toggle', 'loop'].includes(item.trigger)) stopPad(item); else playPad(item);
            });
            control.addEventListener('pointerdown', (event) => { const item = assignments.get(bankKey(bankName, slot)); if (item?.trigger === 'hold' && event.button === 0) { event.preventDefault(); control.focus(); control.setPointerCapture(event.pointerId); playPad(item); } });
            for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) control.addEventListener(name, () => { const item = assignments.get(bankKey(bankName, slot)); if (item?.trigger === 'hold') stopPad(item); });
            control.addEventListener('keydown', (event) => { const item = assignments.get(bankKey(bankName, slot)); if (item?.trigger === 'hold' && ['Enter', ' '].includes(event.key)) { event.preventDefault(); if (!event.repeat) playPad(item); } });
            control.addEventListener('keyup', (event) => { const item = assignments.get(bankKey(bankName, slot)); if (item?.trigger === 'hold' && ['Enter', ' '].includes(event.key)) { event.preventDefault(); stopPad(item); } });
            control.addEventListener('blur', () => { const item = assignments.get(bankKey(bankName, slot)); if (item?.trigger === 'hold') stopPad(item); });
        }
        bank.querySelectorAll('[data-pad-page-button]').forEach((button) => button.addEventListener('click', () => stopPads(bankName)));
        setBankStatus(bankName, Object.keys(padMetadata).some((key) => key.startsWith(`${bankName}:`)) ? 'Dosya yeniden seçilmeli' : 'Ses atanmadı');
    }
    document.getElementById('headphone-gain')?.addEventListener('input', updatePadGains);
    const cueButtons = Array.from(document.querySelectorAll('.headphone-cue-button:not([data-monitor-gui-cue])'));
    const syncCue = (detail = {}) => {
        const available = !!detail.available; const playing = !!detail.playing;
        cueButtons.forEach((button) => { button.disabled = !available; button.setAttribute('aria-pressed', String(playing)); button.title = available ? 'Geçişi ön dinle' : ''; });
        const message = available ? (playing ? 'Çalıyor' : 'Hazır') : '—';
        status('#headphone-monitor-status', message); status('#waveform-headphone-status', message);
    };
    document.addEventListener('radios:preview-source-change', (event) => { if (event.detail?.playing) stopPads(); syncCue(event.detail); });
    cueButtons.forEach((button) => button.addEventListener('click', async () => {
        if (button.disabled) return;
        if (button.getAttribute('aria-pressed') === 'true') window.RadioSTransition?.stop();
        else { stopPads(); try { await window.RadioSTransition?.preview(); } catch { status('#waveform-headphone-status', 'Ön dinleme başlatılamadı'); } }
    }));

    document.addEventListener('click', (event) => {
        const button = event.target instanceof Element ? event.target.closest('button') : null; if (!button || button.disabled) return;
        if (button.matches('[data-open-emergency]')) {
            const form = document.querySelector('[data-emergency-form]'); fields(form, { strategy: settings.emergency.strategy, name: settings.emergency.name }); show('emergency-dialog');
        } else if (button.matches('[data-close-ops-dialog]')) close(button.closest('dialog'));
        else if (button.matches('[data-queue-from-selected]')) { const selected = snapshot()?.selectedItem; addQueue(Array.isArray(selected?.contents) ? selected.contents : [selected]); }
        else if (button.matches('[data-queue-new]')) editQueue(true);
        else if (button.matches('[data-queue-select]')) { selectedId = button.dataset.queueSelect; renderQueue(); }
        else if (button.matches('[data-queue-action]')) {
            const index = queue.findIndex((item) => item.id === selectedId); if (index < 0) return;
            const action = button.dataset.queueAction;
            if (action === 'edit') editQueue();
            else if (action === 'remove') { queue.splice(index, 1); selectedId = queue[Math.min(index, queue.length - 1)]?.id || null; persist(); renderQueue(); record('Öğe geçici sıradan kaldırıldı.'); }
            else { const next = index + (action === 'up' ? -1 : 1); if (next < 0 || next >= queue.length) return; const [item] = queue.splice(index, 1); queue.splice(next, 0, item); persist(); renderQueue(); }
        } else if (button.matches('[data-queue-transfer]')) {
            if (!queue.length) return;
            document.dispatchEvent(new CustomEvent('radios:add-programme-items', { detail: { items: queue.map(({ id, ...item }) => ({ ...item })), playlistName: 'Geçici sıra' } }));
            record(`${queue.length} geçici sıra öğesi program taslağına aktarıldı.`); queue.splice(0); selectedId = null; persist(); renderQueue(); status('[data-queue-status]', 'Aktarıldı.');
        } else if (button.matches('[data-history-tab]')) {
            document.querySelectorAll('[data-history-tab]').forEach((tab) => { const active = tab === button; tab.setAttribute('aria-selected', String(active)); tab.tabIndex = active ? 0 : -1; document.getElementById(tab.getAttribute('aria-controls')).hidden = !active; });
        } else if (button.matches('[data-audit-download]')) download({ type: 'RadioS yerel taslak işlem kaydı', events: audit }, 'radios-taslak-islem-kaydi.json');
        else if (button.matches('[data-rehearsal-toggle]')) {
            if (rehearsalTimer) pauseRehearsal();
            else { rehearsalStart = performance.now(); rehearsalTimer = setInterval(renderRehearsal, 250); status('[data-rehearsal-toggle]', 'Prova duraklat'); renderRehearsal(); }
        } else if (button.matches('[data-rehearsal-reset]')) { pauseRehearsal(); rehearsalElapsed = 0; status('[data-rehearsal-toggle]', 'Prova başlat'); renderRehearsal(); }
        else if (button.matches('[data-open-diagnostic-details]')) show('diagnostic-details-dialog');
        else if (button.matches('[data-open-support-note]')) { close(document.getElementById('diagnostic-details-dialog')); show('support-note-dialog'); }
        else if (button.matches('[data-pad-assign]')) openAssignment(button.dataset.padAssign);
        else if (button.matches('[data-pad-stop]')) stopPads(button.dataset.padStop);
        else if (button.matches('[data-pad-mute]')) { const bank = button.dataset.padMute; if (mutedBanks.has(bank)) mutedBanks.delete(bank); else mutedBanks.add(bank); button.setAttribute('aria-pressed', String(mutedBanks.has(bank))); updatePadGains(); }
        else if (button.matches('[data-pad-solo]')) { soloBank = soloBank === button.dataset.padSolo ? null : button.dataset.padSolo; document.querySelectorAll('[data-pad-solo]').forEach((control) => control.setAttribute('aria-pressed', String(control.dataset.padSolo === soloBank))); updatePadGains(); }
        else if (button.matches('[data-pad-clear]')) {
            const form = button.closest('form'); const bank = form.elements.bank.value; const slot = Number(form.elements.slot.value); const key = bankKey(bank, slot); const item = assignments.get(key); stopPad(item); if (item) URL.revokeObjectURL(item.url); assignments.delete(key); delete padMetadata[key]; write('radios.pad-assignments.v1', padMetadata); renderPad(bank, slot); close(button.closest('dialog')); record(`${bank} ${slot} ataması kaldırıldı.`);
        }
    });
    document.addEventListener('keydown', (event) => {
        const tab = event.target instanceof Element ? event.target.closest('[data-history-tab]') : null; if (!tab) return;
        const tabs = Array.from(document.querySelectorAll('[data-history-tab]')); const index = tabs.indexOf(tab); let next;
        if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') next = (index + 1) % tabs.length;
        if (event.key === 'Home') next = 0; if (event.key === 'End') next = tabs.length - 1;
        if (next !== undefined) { event.preventDefault(); tabs[next].click(); tabs[next].focus(); }
    });
    document.addEventListener('submit', (event) => {
        const form = event.target; if (!(form instanceof HTMLFormElement)) return;
        if (form.matches('[data-queue-form]')) {
            event.preventDefault(); if (!form.reportValidity()) return;
            const current = queue.find((entry) => entry.id === queueEditorId);
            const values = Object.fromEntries(new FormData(form)); const item = cleanItem({ ...current, ...values }); if (!item) return;
            if (current) Object.assign(current, item); else { const entry = { ...item, id: `temporary-${++nextId}` }; queue.push(entry); selectedId = entry.id; }
            persist(); renderQueue(); close(form.closest('dialog')); record('Geçici sıra öğesi kaydedildi.');
        } else if (form.matches('[data-emergency-form]')) {
            event.preventDefault(); saveEmergency({ strategy: form.elements.strategy.value, name: form.elements.name.value }); close(form.closest('dialog'));
        } else if (form.matches('[data-support-note-form]')) {
            event.preventDefault(); const note = text(form.elements.note.value, 1500); if (!form.reportValidity() || !note) return;
            download({ type: 'Yöneticiye bildirim taslağı', created: new Date().toISOString(), note, actualBroadcastState: 'Bilinmiyor', recentDraftActions: audit.slice(0, 10) }, 'radios-bildirim-taslagi.json'); record('Yönetici bildirim taslağı indirildi.'); close(form.closest('dialog'));
        } else if (form.matches('[data-pad-assignment-form]')) {
            event.preventDefault(); const bank = form.elements.bank.value; const slot = Number(form.elements.slot.value); const key = bankKey(bank, slot); const previous = assignments.get(key); const file = form.elements.file.files[0];
            if (!file && !previous) { status('[data-pad-assignment-status]', 'Ses dosyası seçilmedi.'); return; }
            if (file && file.type && !file.type.startsWith('audio/')) { status('[data-pad-assignment-status]', 'Ses dosyası değil.'); return; }
            if (file?.size > 100 * 1024 * 1024) { status('[data-pad-assignment-status]', 'Dosya 100 MB sınırını aşıyor.'); return; }
            const title = text(form.elements.title.value, 100) || (file ? file.name.replace(/\.[^.]+$/, '') : previous.title);
            const trigger = form.elements.trigger.value;
            if (previous) stopPad(previous);
            const audio = file ? new Audio() : previous.audio; const url = file ? URL.createObjectURL(file) : previous.url;
            if (file && previous) URL.revokeObjectURL(previous.url);
            const item = { bank, slot, title, trigger, audio, url, playing: false };
            if (!file) item.duration = previous.duration;
            if (file) {
                audio.src = url; audio.preload = 'metadata';
                audio.addEventListener('loadedmetadata', () => { const active = assignments.get(key); if (active?.audio !== audio) return; active.duration = audio.duration; renderPad(bank, slot); });
                audio.addEventListener('playing', () => { const active = assignments.get(key); if (active?.audio !== audio) return; active.playing = true; renderPad(bank, slot); if (key === bankKey(bank, slot)) setBankStatus(bank, active.title); });
                audio.addEventListener('ended', () => { const active = assignments.get(key); if (active?.audio !== audio) return; active.playing = false; renderPad(bank, slot); if (key === bankKey(bank, slot)) setBankStatus(bank, 'Ön dinleme tamamlandı'); });
                audio.addEventListener('error', () => { const active = assignments.get(key); if (active?.audio !== audio) return; active.playing = false; renderPad(bank, slot); if (key === bankKey(bank, slot)) setBankStatus(bank, 'Dosya okunamadı'); });
            }
            assignments.set(key, item); padMetadata[key] = { title, trigger }; write('radios.pad-assignments.v1', padMetadata); updatePadGains(); renderPad(bank, slot); setBankStatus(bank, 'Atandı'); close(form.closest('dialog')); record(`${bank} ${slot}: ${title} atandı.`);
        }
    });
    document.addEventListener('radios:profile-change', () => { stopPads(); pauseRehearsal(); });
    window.addEventListener('blur', () => stopPads());
    document.addEventListener('visibilitychange', () => { if (document.hidden) stopPads(); });
    window.addEventListener('pagehide', () => { stopPads(); pauseRehearsal(); assignments.forEach((item) => URL.revokeObjectURL(item.url)); });
    renderBed(); renderQueue(); renderAudit(); renderRehearsal(); syncCue({ available: window.RadioSTransition?.canPreview() || false });
})();
