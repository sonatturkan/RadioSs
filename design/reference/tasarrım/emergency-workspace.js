(() => {
    'use strict';
    const workspace = document.querySelector('[data-emergency-workspace]');
    if (!workspace) return;
    const key = 'radios.emergency-workspace.v2';
    const legacyKey = 'radios.emergency-workspace.v1';
    const mime = 'application/x-radios-programme-items';
    const STALE_MS = 15000;
    const OUTPUTS = { studio: 'Stüdyo', internet: 'İnternet', terrestrial: 'Karasal' };
    const TYPES = { music: 'Müzik', announcement: 'Anons', jingle: 'Jingle · kimlik', apology: 'Arıza anonsu', advert: 'Reklam' };
    const MARKS = ['cueStart', 'vocal', 'segue', 'cueOut'];
    const MARK_NAMES = { cueStart: 'Cue başlangıcı', vocal: 'Vokal girişi', segue: 'Sonraki başlangıç', cueOut: 'Çıkış noktası' };
    const STATES = {
        unknown: { label: 'Durum bilinmiyor', icon: '?', level: 'unknown' },
        normal: { label: 'Normal yayın', icon: '✓', level: 'ok' },
        ready: { label: 'Devralmaya hazır', icon: '✓', level: 'ok' },
        active: { label: 'Acil yayında', icon: '!', level: 'active' },
        return: { label: 'Normale dönüş bekleniyor', icon: '↺', level: 'warn' },
    };
    const STEPS = ['normal', 'ready', 'active', 'return'];
    // Thesis 14.1 and DAY-07: each layer is observed separately; a running process is not health.
    const CHAIN = [['engine', 'Motor süreci'], ['scheduler', 'Zamanlayıcı'], ['player', 'Oynatıcı ilerlemesi'], ['audio', 'Ses çıkışı'], ['encoder', 'Encoder / akış'], ['offair', 'Dış dönüş'], ['logger', 'Yayın kaydı'], ['database', 'Veritabanı'], ['media', 'Medya'], ['disk', 'Disk'], ['network', 'Bağlantı'], ['control', 'MIDI / GPIO']];
    const OWNERSHIP = [['engines', 'Aktif / yedek motor'], ['owner', 'Sahip · epoch'], ['phase', 'Yedekleme durumu'], ['probe', 'Son yoklama'], ['failures', 'Hata / eşik'], ['cooldown', 'Bekleme bitişi'], ['operator', 'Operatör gerekli'], ['error', 'Son hata']];
    // States written by SDBASE failover.py.
    const PHASES = { watching: ['İzleniyor', 'ok'], catching_up: ['Eşitleniyor', 'warn'], promotion_pending: ['Terfi bekliyor', 'warn'], promoted: ['Terfi etti', 'active'], blocked: ['Engellendi', 'crit'] };
    const ADVERTS = [['missed', 'Kaçan kuşak'], ['double', 'Çift çalma şüphesi'], ['makeup', 'Telafi bekleyen']];
    // Observation kinds accepted by SDBASE playout.py.
    const OBSERVATIONS = [['audio_present', 'Ses var'], ['silence', 'Sessizlik'], ['off_air', 'Yayın dışı'], ['monitor_error', 'İzleme hatası']];
    const ALARMS = { open: 'Açık', acknowledged: 'Kabul edildi', muted: 'Susturuldu', resolved: 'Çözüldü', recurred: 'Yeniden oluştu' };
    const CANCEL = { false_alarm: 'Yanlış alarm', expired: 'Süresi doldu', source_cancelled: 'Kaynak iptal etti', superseded: 'Yenisi yayımlandı' };
    const SCENARIOS = { silence: 'Sessizlik', engine: 'Motor arızası', database: 'Veritabanı kaybı', network: 'Ağ kaybı', audio: 'Ses çıkışı arızası', power: 'Elektrik kesintisi' };
    const RESULTS = { pass: ['Başarılı', 'ok'], partial: ['Kısmi', 'warn'], fail: ['Başarısız', 'crit'] };
    const ROLES = { viewer: 'Görüntüleme', shift: 'Vardiya sorumlusu', technical: 'Teknik yönetici' };
    const LANGUAGE = /^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8}){0,3}$/;
    const CODE = /^[A-Za-z0-9_.:-]{1,100}$/;

    const text = (value, max = 160) => typeof value === 'string' ? value.trim().slice(0, max) : '';
    const node = (tag, className, value) => { const element = document.createElement(tag); if (className) element.className = className; if (value !== undefined && value !== null) element.textContent = value; return element; };
    const $ = (selector) => workspace.querySelector(selector);
    const $$ = (selector) => Array.from(workspace.querySelectorAll(selector));
    const setText = (selector, value) => { const target = $(selector); if (target) target.textContent = value; };
    const choice = (value, allowed, fallback) => allowed.includes(value) ? value : fallback;
    const integer = (value, min, max) => { const string = typeof value === 'number' ? String(value) : text(value, 12); const number = Number(string); return string !== '' && Number.isInteger(number) && number >= min && number <= max ? number : ''; };
    const uid = (prefix) => `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
    const pad = (value, length = 2) => String(value).padStart(length, '0');
    const minute = (value) => typeof value === 'string' && /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}$/.test(value) && Number.isFinite(Date.parse(`${value}Z`)) ? value : '';
    const instant = (value) => typeof value === 'string' && Number.isFinite(Date.parse(value)) ? value : '';
    // The station clock in the header is UTC, so every time here is UTC as well.
    const utcMinute = (date = new Date()) => date.toISOString().slice(0, 16);
    const clock = (value) => { const date = new Date(typeof value === 'string' && minute(value) ? `${value}Z` : value); return Number.isFinite(date.getTime()) ? date.toLocaleString('tr-TR', { timeZone: 'UTC', day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' }) : '—'; };
    const hourMinute = (value) => new Date(value).toLocaleTimeString('tr-TR', { timeZone: 'UTC', hour: '2-digit', minute: '2-digit' });
    const parseTime = (value) => {
        if (typeof value === 'number') return Number.isFinite(value) && value >= 0 && value <= 86400000 ? Math.round(value) : null;
        const match = /^(\d{1,3}):([0-5]\d)(?:[.,](\d{1,3}))?$/.exec(text(value, 12));
        return match ? (Number(match[1]) * 60 + Number(match[2])) * 1000 + Number((match[3] || '0').padEnd(3, '0')) : null;
    };
    const formatCue = (ms) => ms === null || ms === undefined ? '' : `${pad(Math.floor(ms / 60000))}:${pad(Math.floor(ms % 60000 / 1000))}.${pad(ms % 1000, 3)}`;
    const formatSpan = (ms) => { const seconds = Math.round(ms / 1000); const hours = Math.floor(seconds / 3600); return hours ? `${hours}:${pad(Math.floor(seconds % 3600 / 60))}:${pad(seconds % 60)}` : `${pad(Math.floor(seconds / 60))}:${pad(seconds % 60)}`; };
    const age = (ms) => ms < 60000 ? `${Math.max(0, Math.round(ms / 1000))} sn` : ms < 3600000 ? `${Math.round(ms / 60000)} dk` : `${Math.round(ms / 3600000)} sa`;
    const decibel = (value) => `${value < 0 ? '−' : value > 0 ? '+' : ''}${Math.abs(value).toLocaleString('tr-TR')} dB`;
    const live = (message) => { const region = $('[data-em-live]'); region.textContent = ''; window.setTimeout(() => { region.textContent = message; }, 40); };
    const leveled = (element, level) => { if (level) element.dataset.level = level; else delete element.dataset.level; return element; };

    const cleanItem = (value) => {
        if (!value || typeof value !== 'object' || !text(value.title)) return null;
        const item = { type: choice(value.type, Object.keys(TYPES), 'music'), title: text(value.title), artist: text(value.artist, 120), album: text(value.album, 120), duration: parseTime(value.duration), source: value.source === 'file' ? 'file' : 'manual', fileName: text(value.fileName, 160), fadeMs: integer(value.fadeMs, 0, 20000) };
        // Marks keep the A ≤ V ≤ S ≤ B order and never pass the duration.
        let floor = 0;
        for (const mark of MARKS) {
            const time = parseTime(value[mark]);
            item[mark] = time === null || time < floor || (item.duration !== null && time > item.duration) ? null : time;
            if (item[mark] !== null) floor = item[mark];
        }
        if (!item.fileName) item.source = 'manual';
        return item;
    };
    const codes = (value) => [...new Set((Array.isArray(value) ? value : text(value, 300).split(/[\s,;]+/)).map((code) => text(code, 100)).filter((code) => CODE.test(code)))].sort();
    const outputsOf = (value) => (Array.isArray(value) ? value : []).filter((output, index, all) => Object.hasOwn(OUTPUTS, output) && all.indexOf(output) === index);
    const cleanAlert = (value) => {
        if (!value || typeof value !== 'object') return null;
        const alert = { id: text(value.id, 60) || uid('alert'), purpose: choice(value.purpose, ['test', 'actual'], 'test'), startsAt: minute(value.startsAt), endsAt: minute(value.endsAt), geoCodes: codes(value.geoCodes), outputs: outputsOf(value.outputs), repeatMinutes: integer(value.repeatMinutes, 0, 1440) || 0, text: text(value.text, 4000), language: text(value.language, 35), media: text(value.media, 120), status: choice(value.status, ['draft', 'cancelled'], 'draft'), cancelReason: choice(value.cancelReason, Object.keys(CANCEL), ''), savedAt: instant(value.savedAt) || new Date().toISOString() };
        if (!alert.startsAt || !alert.endsAt || alert.endsAt <= alert.startsAt || !alert.outputs.length || (!alert.text && !alert.media)) return null;
        if (!alert.text) alert.language = ''; else if (!LANGUAGE.test(alert.language)) alert.language = 'tr-TR';
        if (alert.status === 'cancelled' && !alert.cancelReason) alert.cancelReason = 'source_cancelled';
        return alert;
    };
    const cleanDrill = (value) => {
        if (!value || typeof value !== 'object' || !minute(value.at)) return null;
        const drill = { id: text(value.id, 60) || uid('drill'), at: value.at, scenario: choice(value.scenario, Object.keys(SCENARIOS), 'silence'), detect: integer(value.detect, 0, 86400), decide: integer(value.decide, 0, 86400), takeover: integer(value.takeover, 0, 86400), recover: integer(value.recover, 0, 86400), gapMs: integer(value.gapMs, 0, 600000), double: value.double === 'yes' ? 'yes' : 'no', result: choice(value.result, Object.keys(RESULTS), 'pass'), note: text(value.note, 200) };
        return ['detect', 'decide', 'takeover', 'recover', 'gapMs'].every((field) => drill[field] !== '') ? drill : null;
    };
    const collect = (value, clean, limit) => (Array.isArray(value) ? value : []).map(clean).filter(Boolean).slice(0, limit);

    const read = (name) => { try { const parsed = JSON.parse(localStorage.getItem(name)); return parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : null; } catch { return null; } };
    const stored = read(key) || read(legacyKey) || {};
    const previous = stored.settings && typeof stored.settings === 'object' ? stored.settings : {};
    const settings = {
        outputs: outputsOf(Array.isArray(previous.outputs) ? previous.outputs : [previous.output || 'studio']),
        player: text(previous.player, 80), listEnd: choice(previous.listEnd, ['hold', 'repeat'], 'hold'), returnPolicy: choice(previous.returnPolicy, ['manual', 'next'], 'manual'), gpo: text(previous.gpo, 40),
        coverageTarget: choice(Number(previous.coverageTarget), [30, 60, 120, 240, 480], 120),
        silenceEnabled: previous.silenceEnabled === true, monitor: choice(previous.monitor, ['program', 'internet', 'terrestrial', 'studio'], 'program'),
        silenceDb: integer(previous.silenceDb, -90, 0), silenceSeconds: integer(previous.silenceSeconds, 1, 3600),
        fallback: choice(previous.fallback, ['player', 'playlist', 'alarm'], 'playlist'), escalateSeconds: integer(previous.escalateSeconds, 5, 3600), escalateTo: choice(previous.escalateTo, ['playlist', 'alarm', 'none'], 'playlist'),
        restoreDb: integer(previous.restoreDb, -90, 0), restoreSeconds: integer(previous.restoreSeconds, 1, 600), channels: choice(previous.channels, ['both', 'any'], 'both'), measure: choice(previous.measure, ['rms', 'peak'], 'rms'), gpi: text(previous.gpi, 40),
        breakMode: choice(previous.breakMode, ['fade', 'cut', 'wait'], 'fade'), breakFadeMs: integer(previous.breakFadeMs, 0, 10000), breakAfter: choice(previous.breakAfter, ['resume', 'hold'], 'resume'),
        protection: choice(previous.protection, ['confirmation', 'password', 'usb'], 'confirmation'), takeoverRole: choice(previous.takeoverRole, ['shift', 'technical'], 'shift'), configRole: choice(previous.configRole, ['technical', 'shift'], 'technical'),
        breakGlassContact: text(previous.breakGlassContact, 100), breakGlassLocation: text(previous.breakGlassLocation, 120),
        responsible: text(previous.responsible, 100), receiver: text(previous.receiver, 100),
        lastState: text(previous.lastState ?? previous.handoverNote, 1000), actions: text(previous.actions, 1000), risk: text(previous.risk, 1000), licenseNote: text(previous.licenseNote, 600),
        rto: integer(previous.rto, 1, 86400), rpoLog: integer(previous.rpoLog, 0, 1440), rpoCatalog: integer(previous.rpoCatalog, 0, 10080), rpoAudio: integer(previous.rpoAudio, 0, 720),
    };
    const items = collect(stored.items, cleanItem, 500).map((item) => ({ ...item, id: uid('emergency-item') }));
    let selectedId = items[Math.max(0, Math.min(items.length - 1, Number.isInteger(stored.selectedIndex) ? stored.selectedIndex : 0))]?.id || null;
    const quietWindows = collect(stored.quietWindows, (entry) => entry && minute(entry.start) && integer(entry.minutes, 1, 240) !== '' && text(entry.reason, 80) ? { id: text(entry.id, 60) || uid('quiet'), start: entry.start, minutes: integer(entry.minutes, 1, 240), reason: text(entry.reason, 80) } : null, 50);
    const alerts = collect(stored.alerts, cleanAlert, 50);
    const keys = collect(stored.keys, (entry) => entry && text(entry.label, 60) && text(entry.owner, 80) ? { id: text(entry.id, 60) || uid('key'), label: text(entry.label, 60), owner: text(entry.owner, 80), createdAt: instant(entry.createdAt) || new Date().toISOString(), status: choice(entry.status, ['registered', 'revoked'], 'registered') } : null, 50);
    const contacts = collect(stored.contacts, (entry) => entry && text(entry.name, 80) && text(entry.phone, 30) ? { id: text(entry.id, 60) || uid('contact'), name: text(entry.name, 80), role: text(entry.role, 60), phone: text(entry.phone, 30), offHours: entry.offHours === true } : null, 30);
    const drills = collect(stored.drills, cleanDrill, 20);
    const audit = collect(stored.audit, (entry) => entry && typeof entry.message === 'string' && instant(entry.time) ? { time: entry.time, user: text(entry.user, 100), message: text(entry.message, 300) } : null, 200);
    const alarmNotes = stored.alarmNotes && typeof stored.alarmNotes === 'object' ? Object.fromEntries(Object.entries(stored.alarmNotes).filter(([id, state]) => text(id, 80) && ['acknowledged', 'muted'].includes(state)).slice(0, 100)) : {};
    const view = { left: choice(stored.view?.left, ['list', 'queue'], 'list'), side: choice(stored.view?.side, ['takeover', 'silence', 'alert', 'access', 'handover'], 'takeover') };
    let previewGain = typeof stored.previewGain === 'number' && Number.isFinite(stored.previewGain) ? Math.max(-60, Math.min(6, Math.round(stored.previewGain * 2) / 2)) : -12;
    let status = null;
    let statusAt = 0;

    const form = $('[data-emergency-item-form]');
    const list = $('[data-emergency-track-list]');
    const listPanel = $('#em-left-list');
    const sourceSelect = $('[data-emergency-source]');
    const playerSelect = $('[data-emergency-player]');
    const strategy = $('[data-emergency-strategy]');
    const name = $('[data-emergency-name]');
    const canvas = $('[data-em-wave-canvas]');
    const banner = document.querySelector('[data-em-global-banner]');
    const backupSummary = document.querySelector('[data-em-backup-summary]');
    const sources = new Map();
    const audioFiles = new Map();
    const selectedItem = () => items.find((item) => item.id === selectedId);

    const snapshot = () => ({ guiOnly: true, items: items.map(({ id, ...item }) => ({ ...item })), settings: structuredClone(settings), selectedIndex: items.findIndex((item) => item.id === selectedId), previewGain, view: { ...view }, quietWindows: structuredClone(quietWindows), alerts: structuredClone(alerts), keys: structuredClone(keys), contacts: structuredClone(contacts), drills: structuredClone(drills), alarmNotes: { ...alarmNotes }, audit: structuredClone(audit) });
    let storageWarned = false;
    const persist = () => { try { localStorage.setItem(key, JSON.stringify(snapshot())); return true; } catch { if (!storageWarned) live('Tarayıcıya kaydedilemedi.'); storageWarned = true; return false; } };
    const renderAudit = () => {
        const fragment = document.createDocumentFragment();
        for (const entry of audit.slice(0, 60)) {
            const row = node('li'); const time = node('time', '', clock(entry.time)); time.dateTime = entry.time;
            row.append(time, node('span', '', entry.message));
            if (entry.user) row.append(node('small', '', entry.user));
            fragment.append(row);
        }
        $('[data-emergency-audit-list]').replaceChildren(fragment);
    };
    const record = (message) => { audit.unshift({ time: new Date().toISOString(), user: settings.responsible, message: text(message, 300) }); audit.splice(200); persist(); renderAudit(); };

    /* Durum: motor/SDBASE bağlantısı applyStatus ile gelir; gelmezse her şey bilinmiyor kalır. */
    const fresh = () => Boolean(status) && Date.now() - statusAt <= STALE_MS;
    const takeoverState = () => status ? choice(status.takeover, STEPS, 'unknown') : 'unknown';
    const fileState = (item) => { const file = status?.files?.[item.title]; if (!file || typeof file !== 'object') return null; return { local: file.local === true, standby: file.standby === true }; };
    const effective = (item) => { const end = item.segue ?? item.cueOut ?? item.duration; return end === null ? null : Math.max(0, end - (item.cueStart ?? 0)); };
    const coverage = () => {
        const known = items.reduce((sum, item) => sum + (effective(item) ?? 0), 0);
        const unknown = items.filter((item) => effective(item) === null).length;
        const states = items.map(fileState);
        const verified = states.filter((file) => file?.local && file?.standby).length;
        return { known, unknown, verified, checked: states.some(Boolean), short: settings.listEnd !== 'repeat' && known < settings.coverageTarget * 60000 };
    };
    const behaviour = () => {
        const outputs = settings.outputs.map((output) => OUTPUTS[output]).join(', ') || 'Çıkış seçilmedi';
        const player = settings.player && playerSelect.selectedOptions[0]?.value ? playerSelect.selectedOptions[0].textContent : 'Oynatıcı seçilmedi';
        const parts = [[`${outputs} → `, player]];
        if (strategy.value === 'playlist') parts.push([`“${text(name.value, 100) || 'Acil liste'}” başlar`], [settings.listEnd === 'repeat' ? 'liste tekrarlanır' : 'liste bitince operatör beklenir']);
        else parts.push([strategy.value === 'next' ? 'sıradaki olaya geçer' : 'çalan öğe sürer']);
        parts.push([settings.returnPolicy === 'manual' ? 'dönüş operatörle' : 'dönüş öğe sonunda hazırlanır']);
        return parts;
    };
    const renderCommand = () => {
        const state = takeoverState(); const meta = STATES[state]; const stale = Boolean(status) && !fresh();
        const command = $('[data-em-command]'); command.dataset.level = meta.level; command.dataset.stale = String(stale);
        setText('[data-em-state-icon]', meta.icon); setText('[data-em-state-label]', stale ? `${meta.label} · eski bilgi` : meta.label);
        const current = STEPS.indexOf(state);
        $$('[data-em-step]').forEach((step, index) => { step.classList.toggle('is-current', index === current); step.classList.toggle('is-done', current > -1 && index < current); if (index === current) step.setAttribute('aria-current', 'step'); else step.removeAttribute('aria-current'); });
        const target = $('[data-em-behavior]'); target.replaceChildren(...behaviour().map(([lead, strong]) => { const span = node('span', '', lead); if (strong) span.append(node('strong', '', strong)); return span; }));
        const live = state === 'active' && fresh();
        for (const button of $$('[data-em-return], [data-em-abort]')) { button.disabled = !live; if (!live) disarm(button); }
        if (banner) {
            banner.hidden = state !== 'active'; banner.dataset.stale = String(stale);
            const detail = banner.querySelector('[data-em-global-text]');
            if (detail) detail.textContent = behaviour().map(([lead, strong]) => `${lead}${strong || ''}`).join(' · ') + (stale ? ` · bilgi ${age(Date.now() - statusAt)} önce` : '');
        }
    };
    const chip = (name, value, level) => { const target = $(`[data-em-chip="${name}"]`); target.querySelector('.em-chip-value').textContent = value; leveled(target, level); };
    const renderChips = () => {
        const own = status?.ownership;
        if (own && typeof own === 'object') {
            const phase = PHASES[own.state];
            chip('ownership', `${text(own.owner, 40) || '—'}${Number.isInteger(own.epoch) ? ` · e${own.epoch}` : ''}`, own.operatorRequired ? 'crit' : phase?.[1] || 'unknown');
        } else chip('ownership', '—', 'unknown');
        const cover = coverage();
        if (!items.length) chip('coverage', 'Liste boş', 'crit');
        else chip('coverage', `${formatSpan(cover.known)}${cover.checked ? ` · ${cover.verified}/${items.length}` : ''}`, cover.short || (cover.checked && cover.verified < items.length) ? 'warn' : cover.checked ? 'ok' : 'unknown');
        const complete = settings.silenceDb !== '' && settings.silenceSeconds !== '';
        const last = status?.silence?.last;
        chip('silence', !settings.silenceEnabled ? 'Kapalı' : !complete ? 'Eksik' : `${decibel(settings.silenceDb)} · ${settings.silenceSeconds} sn`, !settings.silenceEnabled || !complete ? 'warn' : last === 'silence' || last === 'off_air' ? 'crit' : last === 'monitor_error' ? 'warn' : last === 'audio_present' ? 'ok' : 'unknown');
        const chain = CHAIN.map(([layer]) => status?.chain?.[layer]).filter((entry) => entry && typeof entry === 'object');
        const worst = ['crit', 'warn', 'ok'].find((level) => chain.some((entry) => entry.level === level));
        chip('chain', chain.length ? `${chain.filter((entry) => entry.level === 'ok').length}/${CHAIN.length}` : '—', worst || 'unknown');
        const drafts = alerts.filter((alert) => alert.status === 'draft');
        const activeAlert = status?.alerts?.active;
        chip('alert', Number.isInteger(activeAlert) && activeAlert > 0 ? `${activeAlert} yayında` : drafts.length ? `${drafts.length} taslak` : '—', Number.isInteger(activeAlert) && activeAlert > 0 ? 'active' : drafts.some((alert) => alert.purpose === 'actual') ? 'warn' : 'unknown');
        const drill = drills[0];
        chip('drill', drill ? `${clock(drill.at)} · ${RESULTS[drill.result][0]}` : 'Yok', drill ? RESULTS[drill.result][1] : 'warn');
        chip('fresh', status ? `${age(Date.now() - statusAt)} önce` : '—', !status ? 'unknown' : fresh() ? 'ok' : 'warn');
        if (backupSummary) {
            const state = takeoverState();
            const [value, level] = state === 'active' ? ['Acil yayında', 'active'] : status && (own?.operatorRequired || own?.state === 'blocked') ? ['Sorun', 'crit'] : !items.length ? ['Acil liste boş', 'warn'] : fresh() && state === 'ready' ? ['Hazır', 'ok'] : ['Bilinmiyor', ''];
            backupSummary.textContent = value; leveled(backupSummary, level);
        }
    };
    const facts = (selector, entries) => {
        const fragment = document.createDocumentFragment();
        for (const [label, value, level] of entries) { const row = node('div'); row.append(node('dt', '', label), leveled(node('dd', '', value || '—'), level)); fragment.append(row); }
        $(selector).replaceChildren(fragment);
    };
    const renderStatusFacts = () => {
        const own = status?.ownership || {};
        const phase = PHASES[own.state];
        const values = {
            engines: own.activeEngine || own.standbyEngine ? `${text(own.activeEngine, 40) || '—'} / ${text(own.standbyEngine, 40) || '—'}` : '',
            owner: own.owner ? `${text(own.owner, 40)}${Number.isInteger(own.epoch) ? ` · ${own.epoch}` : ''}` : '',
            phase: phase?.[0] || '', probe: instant(own.lastProbeAt) ? `${hourMinute(own.lastProbeAt)} · ${age(Date.now() - Date.parse(own.lastProbeAt))} önce` : '',
            failures: Number.isInteger(own.failures) ? `${own.failures} / ${Number.isInteger(own.threshold) ? own.threshold : '—'}` : '',
            cooldown: instant(own.cooldownUntil) ? hourMinute(own.cooldownUntil) : '', operator: typeof own.operatorRequired === 'boolean' ? (own.operatorRequired ? 'Evet' : 'Hayır') : '', error: text(own.lastError, 200),
        };
        facts('[data-em-ownership]', OWNERSHIP.map(([field, label]) => [label, values[field], field === 'phase' ? phase?.[1] : field === 'operator' && own.operatorRequired ? 'crit' : '']));
        const adverts = status?.adverts || {};
        facts('[data-em-adverts]', ADVERTS.map(([field, label]) => [label, Number.isInteger(adverts[field]) ? String(adverts[field]) : '', adverts[field] > 0 ? 'warn' : '']));
        const silence = status?.silence || {};
        facts('[data-em-obs]', OBSERVATIONS.map(([kind, label]) => [label, instant(silence[kind]) ? hourMinute(silence[kind]) : '', silence.last === kind ? (kind === 'audio_present' ? 'ok' : 'crit') : '']).concat([['Son tetik / sonuç', silence.lastTrigger && instant(silence.lastTrigger.at) ? `${hourMinute(silence.lastTrigger.at)} · ${text(silence.lastTrigger.result, 60) || '—'}` : '', '']]));
        const level = typeof silence.levelDb === 'number' && Number.isFinite(silence.levelDb) ? Math.max(-90, Math.min(0, silence.levelDb)) : null;
        const meter = $('[data-em-meter]'); $('[data-em-meter-bar]').style.width = level === null ? '0' : `${(level + 90) / 90 * 100}%`;
        if (level === null) meter.removeAttribute('aria-valuenow'); else meter.setAttribute('aria-valuenow', String(level));
        setText('[data-em-meter-value]', level === null ? '— dB' : decibel(Math.round(level)));
        setText('[data-em-session-role]', ROLES[status?.session?.role] || '—');
        const license = status?.license?.standby;
        const licenseTarget = $('[data-em-license]'); licenseTarget.textContent = license === true ? 'Etkin' : license === false ? 'Etkin değil' : '—'; leveled(licenseTarget, license === false ? 'crit' : '');
    };
    const renderChain = () => {
        const fragment = document.createDocumentFragment();
        for (const [layer, label] of CHAIN) {
            const entry = status?.chain?.[layer] && typeof status.chain[layer] === 'object' ? status.chain[layer] : {};
            const row = leveled(node('li'), choice(entry.level, ['ok', 'warn', 'crit'], 'unknown'));
            row.append(node('span', 'em-chain-name', label), node('span', 'em-chain-value', text(entry.text, 120) || '—'));
            const alarm = entry.alarm && typeof entry.alarm === 'object' && text(entry.alarm.id, 80) ? entry.alarm : null;
            if (alarm) {
                const state = alarmNotes[alarm.id] && ['open', 'recurred'].includes(alarm.state) ? alarmNotes[alarm.id] : choice(alarm.state, Object.keys(ALARMS), 'open');
                row.append(node('span', 'em-chain-detail', [text(alarm.impact, 140), text(alarm.action, 140), ALARMS[state]].filter(Boolean).join(' → ')));
                if (['open', 'recurred'].includes(state)) {
                    const actions = node('div', 'em-row');
                    for (const [action, caption] of [['acknowledged', 'Kabul et'], ['muted', 'Sustur']]) { const button = node('button', '', caption); button.type = 'button'; button.dataset.emAlarm = alarm.id; button.dataset.emAlarmAction = action; button.dataset.emAlarmLayer = label; actions.append(button); }
                    row.append(actions);
                }
            }
            fragment.append(row);
        }
        $('[data-em-chain-rows]').replaceChildren(fragment);
    };
    const renderStatus = () => { renderCommand(); renderChips(); renderStatusFacts(); renderChain(); renderCoverage(); renderList(); };

    /* Acil liste */
    const renderCoverage = () => {
        const cover = coverage();
        const total = $('[data-em-metric="total"]'); total.textContent = items.length ? `${formatSpan(cover.known)}${cover.unknown ? ` + ${cover.unknown} ?` : ''}` : '—'; leveled(total, items.length && cover.short ? 'warn' : '');
        setText('[data-em-metric="until"]', !items.length ? '—' : settings.listEnd === 'repeat' ? 'Tekrarla süresiz' : `≈ ${hourMinute(Date.now() + cover.known)}`);
        const verified = $('[data-em-metric="verified"]'); verified.textContent = items.length && cover.checked ? `${cover.verified} / ${items.length}` : '—'; leveled(verified, cover.checked && cover.verified < items.length ? 'warn' : '');
    };
    const renderList = () => {
        const focused = list.contains(document.activeElement) ? { id: document.activeElement.dataset.emergencySelect || document.activeElement.dataset.emergencyRowCue || document.activeElement.dataset.emergencyRemove, type: document.activeElement.dataset.emergencySelect ? 'select' : document.activeElement.dataset.emergencyRowCue ? 'row-cue' : 'remove' } : null;
        const fragment = document.createDocumentFragment();
        items.forEach((item, index) => {
            const row = node('li', `em-track${item.id === selectedId ? ' is-selected' : ''}`); row.dataset.emergencyItem = item.id;
            const number = node('span', 'em-track-number', String(index + 1)); number.setAttribute('aria-hidden', 'true');
            const select = node('button', 'em-track-select'); select.type = 'button'; select.dataset.emergencySelect = item.id; select.setAttribute('aria-pressed', String(item.id === selectedId));
            select.append(node('strong', '', item.title), node('small', '', [TYPES[item.type], item.artist, item.album].filter(Boolean).join(' · '))); select.title = `${index + 1}. ${item.title}`;
            const file = fileState(item);
            const fileMark = leveled(node('span', 'em-file-state', !file ? '○' : file.local && file.standby ? '●' : file.local || file.standby ? '◐' : '×'), !file ? '' : file.local && file.standby ? 'ok' : file.local || file.standby ? 'warn' : 'crit');
            const fileLabel = !file ? 'Dosya doğrulanmadı' : `Yerel ${file.local ? 'var' : 'yok'} · yedek uç ${file.standby ? 'var' : 'yok'}`; fileMark.title = fileLabel; fileMark.setAttribute('aria-label', fileLabel); fileMark.setAttribute('role', 'img');
            const side = node('div', 'em-track-side');
            const cue = node('button', 'em-cue', 'CUE'); cue.type = 'button'; cue.dataset.emergencyRowCue = item.id; cue.disabled = !audioFiles.has(item.id); cue.setAttribute('aria-pressed', String(playingId === item.id)); cue.setAttribute('aria-label', `${item.title} ön dinle`);
            const remove = node('button', '', '×'); remove.type = 'button'; remove.dataset.emergencyRemove = item.id; remove.setAttribute('aria-label', `${item.title} öğesini kaldır`);
            side.append(fileMark, node('span', '', effective(item) === null ? '—' : formatSpan(effective(item))), cue, remove); row.append(number, select, side); fragment.append(row);
        });
        list.replaceChildren(fragment);
        if (focused) list.querySelector(`[data-emergency-${focused.type}="${focused.id}"]`)?.focus({ preventScroll: true });
        const index = items.findIndex((item) => item.id === selectedId);
        $$('[data-emergency-list-action]').forEach((button) => { button.disabled = index < 0 || (button.dataset.emergencyListAction === 'up' && index === 0) || (button.dataset.emergencyListAction === 'down' && index === items.length - 1); });
        setText('[data-emergency-list-count]', String(items.length));
        renderCoverage();
    };
    const refreshPlayers = () => {
        const fragment = document.createDocumentFragment(); const blank = node('option', '', 'Oynatıcı seç'); blank.value = ''; fragment.append(blank);
        document.querySelectorAll('[data-live-player]').forEach((card) => { const id = text(card.dataset.livePlayer, 80); if (!id) return; const option = node('option', '', text(card.querySelector('h3')?.textContent, 100) || id); option.value = id; fragment.append(option); });
        playerSelect.replaceChildren(fragment); playerSelect.value = settings.player;
        if (playerSelect.value !== settings.player) { settings.player = ''; playerSelect.value = ''; persist(); }
    };
    const refreshSources = () => {
        const current = sourceSelect.value; sources.clear(); const fragment = document.createDocumentFragment(); const blank = node('option', '', 'Program / playlist'); blank.value = ''; fragment.append(blank);
        const programme = window.RadioSProgramme?.getSnapshot()?.programme;
        if (programme && Array.isArray(programme.items)) sources.set('programme', { name: text(programme.name) || 'Program taslağı', items: programme.items.map(cleanItem).filter(Boolean) });
        const playlists = window.RadioSPlaylist?.snapshot()?.playlists;
        (Array.isArray(playlists) ? playlists : []).forEach((playlist) => { const id = text(playlist.id, 100); if (!id) return; sources.set(`playlist:${id}`, { name: text(playlist.name) || 'Adsız playlist', items: (Array.isArray(playlist.items) ? playlist.items : []).map(cleanItem).filter(Boolean) }); });
        for (const [value, source] of sources) { const option = node('option', '', `${source.name} · ${source.items.length}`); option.value = value; option.disabled = !source.items.length; fragment.append(option); }
        sourceSelect.replaceChildren(fragment); sourceSelect.value = sources.has(current) ? current : '';
        $('[data-emergency-add-source]').disabled = !sources.get(sourceSelect.value)?.items.length;
        $('[data-emergency-add-selected]').disabled = !cleanItem(window.RadioSProgramme?.getSnapshot()?.selectedItem);
        refreshPlayers(); renderCommand();
    };
    const addItems = (values, sourceName = '') => {
        const added = (Array.isArray(values) ? values : []).map(cleanItem).filter(Boolean).map((item) => ({ ...item, id: uid('emergency-item') }));
        if (!added.length) { live('Eklenecek geçerli öğe yok.'); return; }
        items.push(...added); selectedId = added[0].id; stopPreview(); record(`${added.length} öğe acil listeye eklendi${sourceName ? `: ${text(sourceName, 100)}` : ''}.`); renderList(); fillItem(); renderChips();
    };
    const removeItem = (id) => {
        const index = items.findIndex((item) => item.id === id); if (index < 0) return;
        const [removed] = items.splice(index, 1); if (playingId === id) stopPreview(); audioFiles.delete(id);
        if (selectedId === id) selectedId = items[Math.min(index, items.length - 1)]?.id || null;
        record(`${removed.title} acil listeden kaldırıldı.`); renderList(); fillItem(); renderChips();
    };

    /* Öğe, dalga biçimi ve cue işaretleri */
    const fileKey = () => selectedId || 'draft';
    const formDuration = () => { const loaded = audioFiles.get(fileKey()); return loaded ? Math.round(loaded.buffer.duration * 1000) : parseTime(form.elements.duration.value); };
    const marksFromForm = () => Object.fromEntries(MARKS.map((mark) => [mark, parseTime(form.elements[mark].value)]));
    const drawWave = () => {
        const ratio = Math.max(1, Math.min(3, window.devicePixelRatio || 1));
        const width = Math.max(300, Math.round(canvas.clientWidth * ratio)); const height = Math.max(60, Math.round(canvas.clientHeight * ratio));
        if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
        const drawing = canvas.getContext('2d'); if (!drawing) return;
        drawing.fillStyle = '#040404'; drawing.fillRect(0, 0, width, height);
        const loaded = audioFiles.get(fileKey()); const duration = formDuration();
        drawing.strokeStyle = '#262626'; drawing.lineWidth = 1; drawing.beginPath(); drawing.moveTo(0, height / 2); drawing.lineTo(width, height / 2); drawing.stroke();
        if (duration > 0) {
            // Ruler only; it is not a measured waveform.
            const step = [1, 5, 10, 15, 30, 60, 120, 300, 600].map((seconds) => seconds * 1000).find((value) => duration / value <= 12) || 600000;
            drawing.fillStyle = '#2e2e2e';
            for (let time = step; time < duration; time += step) drawing.fillRect(Math.round(time / duration * width), height - 6 * ratio, 1, 6 * ratio);
        }
        if (loaded) {
            const data = loaded.buffer.getChannelData(0); const stride = data.length / width; const increment = Math.max(1, Math.floor(stride / 256));
            drawing.fillStyle = '#c4c4c4';
            for (let x = 0; x < width; x++) {
                let min = 0, max = 0; const first = Math.floor(x * stride), end = Math.min(data.length, Math.floor((x + 1) * stride));
                for (let index = first; index < end; index += increment) { const sample = data[index]; if (sample < min) min = sample; if (sample > max) max = sample; }
                const top = height / 2 - max * (height / 2 - 4); drawing.fillRect(x, top, 1, Math.max(1, (max - min) * (height / 2 - 4)));
            }
        }
        const marks = marksFromForm();
        if (duration > 0 && (marks.cueStart !== null || marks.cueOut !== null)) {
            drawing.fillStyle = 'rgba(0, 0, 0, .62)';
            if (marks.cueStart) drawing.fillRect(0, 0, Math.min(1, marks.cueStart / duration) * width, height);
            if (marks.cueOut !== null && marks.cueOut < duration) { const x = Math.min(1, marks.cueOut / duration) * width; drawing.fillRect(x, 0, width - x, height); }
        }
        canvas.setAttribute('aria-label', loaded ? `${loaded.name} · dalga biçimi` : 'Dalga biçimi yok');
    };
    const renderMarkers = () => {
        const duration = formDuration(); const marks = marksFromForm();
        for (const mark of MARKS) {
            const marker = $(`[data-em-marker="${mark}"]`); const time = marks[mark];
            marker.hidden = !(duration > 0 && time !== null && time <= duration);
            if (!marker.hidden) { marker.style.left = `${time / duration * 100}%`; marker.title = `${MARK_NAMES[mark]} ${formatCue(time)}`; marker.setAttribute('aria-valuetext', formatCue(time)); }
        }
        setText('[data-em-wave-end]', duration > 0 ? formatSpan(duration) : '');
        drawWave();
    };
    const renderFile = () => {
        const loaded = audioFiles.get(fileKey());
        setText('[data-em-file-info]', loaded ? loaded.name : selectedItem()?.fileName ? `${selectedItem().fileName} · yeniden yükle` : '');
        form.elements.duration.readOnly = Boolean(loaded);
        const cue = $('[data-emergency-cue]'); cue.disabled = !loaded; cue.setAttribute('aria-pressed', String(playingId === fileKey() && Boolean(loaded)));
    };
    const fillItem = () => {
        const item = selectedItem();
        for (const field of ['title', 'artist', 'album', 'type', 'fadeMs']) { form.elements[field].value = item?.[field] ?? (field === 'type' ? 'music' : ''); form.elements[field].setCustomValidity(''); }
        for (const field of ['duration', ...MARKS]) { form.elements[field].value = formatCue(item?.[field] ?? null); form.elements[field].setCustomValidity(''); }
        setText('[data-em-editor-title]', item ? item.title : 'Yeni öğe');
        setText('[data-emergency-item-save]', item ? 'Kaydet' : 'Sıraya ekle');
        $('[data-em-file]').value = '';
        renderGain(); renderFile(); renderMarkers();
    };
    const renderGain = () => {
        const range = $('[data-emergency-preview-volume]'); range.value = String(previewGain); range.setAttribute('aria-valuetext', decibel(previewGain));
        setText('[data-emergency-preview-volume-value]', decibel(previewGain));
        if (previewNodes?.gain && audioContext) previewNodes.gain.gain.setTargetAtTime(Math.pow(10, previewGain / 20), audioContext.currentTime, 0.03);
    };
    const validateMarks = () => {
        const duration = formDuration(); let floor = null; let valid = true;
        for (const mark of MARKS) {
            const input = form.elements[mark]; const raw = input.value.trim(); const time = parseTime(raw);
            const message = raw && time === null ? 'Biçim mm:ss.mmm olmalı.' : time !== null && duration !== null && time > duration ? 'İşaret süreyi aşamaz.' : time !== null && floor !== null && time < floor ? 'İşaret sırası A ≤ V ≤ S ≤ B olmalı.' : '';
            input.setCustomValidity(message); if (message) valid = false; if (time !== null) floor = time;
        }
        const rawDuration = form.elements.duration.value.trim(); form.elements.duration.setCustomValidity(rawDuration && parseTime(rawDuration) === null ? 'Biçim mm:ss.mmm olmalı.' : '');
        return valid;
    };
    let drag = null;
    const moveMarker = (mark, time) => {
        const duration = formDuration(); if (!(duration > 0)) return;
        const marks = marksFromForm(); const order = MARKS.indexOf(mark);
        const low = MARKS.slice(0, order).map((other) => marks[other]).filter((value) => value !== null);
        const high = MARKS.slice(order + 1).map((other) => marks[other]).filter((value) => value !== null);
        const clamped = Math.round(Math.max(low.length ? Math.max(...low) : 0, Math.min(high.length ? Math.min(...high) : duration, time)) / 10) * 10;
        form.elements[mark].value = formatCue(clamped); form.elements[mark].setCustomValidity(''); renderMarkers();
    };
    $('[data-em-markers]').addEventListener('pointerdown', (event) => {
        const marker = event.target.closest('[data-em-marker]'); if (!marker) return;
        event.preventDefault(); marker.setPointerCapture(event.pointerId); marker.focus({ preventScroll: true });
        drag = { mark: marker.dataset.emMarker, rect: canvas.getBoundingClientRect() };
    });
    $('[data-em-markers]').addEventListener('pointermove', (event) => { if (!drag) return; moveMarker(drag.mark, (event.clientX - drag.rect.left) / drag.rect.width * formDuration()); });
    for (const name of ['pointerup', 'pointercancel']) $('[data-em-markers]').addEventListener(name, () => { drag = null; });
    $('[data-em-markers]').addEventListener('keydown', (event) => {
        const marker = event.target.closest('[data-em-marker]'); if (!marker || !['ArrowLeft', 'ArrowRight'].includes(event.key)) return;
        event.preventDefault(); const step = event.shiftKey ? 1000 : event.altKey ? 10 : 100;
        moveMarker(marker.dataset.emMarker, (parseTime(form.elements[marker.dataset.emMarker].value) ?? 0) + (event.key === 'ArrowLeft' ? -step : step));
    });

    /* Kulaklık ön dinlemesi: yalnız bu bilgisayarın ses çıkışı (UI-04) */
    let audioContext = null;
    let previewNodes = null;
    let playingId = null;
    const ensureContext = () => { if (!audioContext || audioContext.state === 'closed') { const Context = window.AudioContext || window.webkitAudioContext; if (!Context) throw new Error('Tarayıcı ses çözümlemeyi desteklemiyor.'); audioContext = new Context(); } return audioContext; };
    const stopPreview = () => {
        if (previewNodes) { previewNodes.source.onended = null; try { previewNodes.source.stop(); } catch {} previewNodes.source.disconnect(); previewNodes.gain.disconnect(); }
        previewNodes = null; const was = playingId; playingId = null;
        if (was) { renderFile(); $$('[data-emergency-row-cue]').forEach((button) => button.setAttribute('aria-pressed', 'false')); }
    };
    const startPreview = async (id) => {
        const loaded = audioFiles.get(id); if (!loaded) return;
        stopPreview();
        try {
            const context = ensureContext(); if (context.state === 'suspended') await context.resume();
            const item = id === fileKey() ? { cueStart: parseTime(form.elements.cueStart.value), cueOut: parseTime(form.elements.cueOut.value) } : items.find((entry) => entry.id === id) || {};
            const start = (item.cueStart ?? 0) / 1000; const end = Math.min(loaded.buffer.duration, (item.cueOut ?? loaded.buffer.duration * 1000) / 1000);
            if (end - start <= 0.05) return;
            const source = context.createBufferSource(); const gain = context.createGain();
            source.buffer = loaded.buffer; gain.gain.value = Math.pow(10, previewGain / 20); source.connect(gain).connect(context.destination);
            source.onended = () => { if (previewNodes?.source === source) stopPreview(); };
            source.start(0, start, end - start); previewNodes = { source, gain }; playingId = id;
            renderFile(); $$('[data-emergency-row-cue]').forEach((button) => button.setAttribute('aria-pressed', String(button.dataset.emergencyRowCue === id)));
        } catch (error) { stopPreview(); live(`Ön dinleme açılamadı: ${error.message || 'bilinmeyen hata'}`); }
    };
    $('[data-em-file]').addEventListener('change', async (event) => {
        const file = event.target.files?.[0]; if (!file) return;
        if (file.size > 150 * 1024 * 1024) { event.target.value = ''; live('Dosya 150 MB sınırını aşıyor.'); return; }
        const target = fileKey();
        try {
            const buffer = await ensureContext().decodeAudioData(await file.arrayBuffer());
            if (!Number.isFinite(buffer.duration) || buffer.duration < 0.1 || buffer.duration > 86400) throw new Error('Süre desteklenen aralıkta değil.');
            if (target !== fileKey()) return;
            stopPreview(); audioFiles.set(target, { buffer, name: text(file.name, 160) });
            const duration = Math.round(buffer.duration * 1000); form.elements.duration.value = formatCue(duration);
            for (const mark of MARKS) if ((parseTime(form.elements[mark].value) ?? 0) > duration) form.elements[mark].value = '';
            if (!form.elements.title.value.trim()) form.elements.title.value = text(file.name.replace(/\.[^.]+$/, ''), 160);
            renderFile(); renderMarkers(); renderList(); live(`${file.name} yüklendi.`);
        } catch (error) { event.target.value = ''; live(`Ses dosyası açılamadı: ${error.message || 'desteklenmeyen biçim'}`); }
    });
    window.addEventListener('resize', () => { if (!workspace.hidden) drawWave(); });

    /* Ayarlar: tek bağlama, değişiklik geçmişi önceki/yeni değerle (GUV-05) */
    const labelOf = (input) => {
        if (input.dataset.emSetting === 'outputs') return 'Etkilenecek çıkışlar';
        const label = input.closest('label'); if (!label) return input.dataset.emSetting;
        const copy = label.cloneNode(true); copy.querySelectorAll('input, select, textarea').forEach((control) => control.remove()); return copy.textContent.replace(/\s+/g, ' ').trim();
    };
    const display = (input, value) => {
        if (input.dataset.emSetting === 'outputs') return value.map((output) => OUTPUTS[output]).join(', ') || '—';
        if (input.type === 'checkbox') return value ? 'Açık' : 'Kapalı';
        if (input.tagName === 'SELECT') return Array.from(input.options).find((option) => option.value === String(value))?.textContent || '—';
        return value === '' ? '—' : String(value);
    };
    const readSetting = (input) => {
        const field = input.dataset.emSetting;
        if (field === 'outputs') return $$('[data-em-setting="outputs"]').filter((box) => box.checked).map((box) => box.value);
        if (input.type === 'checkbox') return input.checked;
        if (input.type === 'number') return integer(input.value, Number(input.min), Number(input.max));
        if (input.tagName === 'SELECT') { const value = field === 'coverageTarget' ? Number(input.value) : input.value; return field === 'player' ? text(value, 80) : value; }
        return text(input.value, input.maxLength > 0 ? input.maxLength : 160);
    };
    const AUDITED = new Set(['outputs', 'player', 'listEnd', 'returnPolicy', 'gpo', 'coverageTarget', 'silenceEnabled', 'monitor', 'silenceDb', 'silenceSeconds', 'fallback', 'escalateSeconds', 'escalateTo', 'restoreDb', 'restoreSeconds', 'channels', 'measure', 'gpi', 'breakMode', 'breakFadeMs', 'breakAfter', 'protection', 'takeoverRole', 'configRole', 'rto', 'rpoLog', 'rpoCatalog', 'rpoAudio']);
    const writeSettings = () => {
        $$('[data-em-setting]').forEach((input) => {
            const field = input.dataset.emSetting;
            if (field === 'outputs') input.checked = settings.outputs.includes(input.value);
            else if (input.type === 'checkbox') input.checked = settings[field] === true;
            else input.value = String(settings[field] ?? '');
        });
    };
    const settingsChanged = () => { renderCommand(); renderChips(); renderCoverage(); persist(); };
    workspace.addEventListener('input', (event) => {
        const input = event.target.closest?.('[data-em-setting]'); if (!input || input.type === 'checkbox' || input.tagName === 'SELECT') return;
        input.setCustomValidity(''); settings[input.dataset.emSetting] = readSetting(input); settingsChanged();
    });
    const committed = new Map();
    workspace.addEventListener('focusin', (event) => { const input = event.target.closest?.('[data-em-setting]'); if (input && !committed.has(input)) committed.set(input, settings[input.dataset.emSetting]); });
    workspace.addEventListener('change', (event) => {
        const input = event.target.closest?.('[data-em-setting]'); if (!input) return;
        const field = input.dataset.emSetting; const before = committed.has(input) ? committed.get(input) : settings[field];
        input.setCustomValidity(''); settings[field] = readSetting(input); committed.delete(input);
        if (AUDITED.has(field) && JSON.stringify(before) !== JSON.stringify(settings[field])) record(`${labelOf(input)}: ${display(input, before)} → ${display(input, settings[field])}`);
        settingsChanged();
    });

    /* Bölümler ve çipler */
    const activateTabs = (attribute, value, focus = false) => {
        $$(`[${attribute}]`).forEach((tab) => { const selected = tab.getAttribute(attribute) === value; tab.setAttribute('aria-selected', String(selected)); tab.tabIndex = selected ? 0 : -1; const panel = document.getElementById(tab.getAttribute('aria-controls')); if (panel) panel.hidden = !selected; if (selected && focus) tab.focus(); });
    };
    const showSide = (panel, focus = false) => { view.side = panel; activateTabs('data-em-tab', panel, focus); persist(); };
    const showLeft = (panel, focus = false) => { view.left = panel; activateTabs('data-em-left-tab', panel, focus); persist(); };
    for (const attribute of ['data-em-tab', 'data-em-left-tab']) {
        workspace.addEventListener('keydown', (event) => {
            const tab = event.target.closest?.(`[${attribute}]`); if (!tab || !['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
            const tabs = $$(`[${attribute}]`); const index = tabs.indexOf(tab); event.preventDefault();
            const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (index + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
            (attribute === 'data-em-tab' ? showSide : showLeft)(tabs[next].getAttribute(attribute), true);
        });
    }
    const goto = (target, chipButton) => {
        if (target === 'chain') { const chain = $('[data-em-chain]'); chain.hidden = !chain.hidden; chipButton.setAttribute('aria-expanded', String(!chain.hidden)); return; }
        if (target === 'list') { showLeft('list'); list.querySelector('button')?.focus(); return; }
        showSide(target, true); $('.em-settings').scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    };
    const disarm = (button) => { window.clearTimeout(button._emTimer); if (button.classList.contains('is-armed')) { button.classList.remove('is-armed'); button.textContent = button.dataset.emCaption; } };
    const arm = (button, command) => {
        // Protected two-step command: the first press arms, the second within four seconds sends.
        if (!button.classList.contains('is-armed')) { button.dataset.emCaption = button.textContent; button.classList.add('is-armed'); button.textContent = `Onayla: ${button.dataset.emCaption}`; button._emTimer = window.setTimeout(() => disarm(button), 4000); return; }
        disarm(button); record(`${button.textContent} komutu gönderildi.`);
        document.dispatchEvent(new CustomEvent('radios:emergency-command', { detail: { command, outputs: [...settings.outputs], at: new Date().toISOString() } }));
    };

    const syncEmergency = (detail) => {
        if (!detail || typeof detail !== 'object') return;
        strategy.value = choice(detail.strategy, ['playlist', 'next', 'continue'], 'playlist'); name.value = text(detail.name, 100);
        $('[data-em-name-field]').hidden = strategy.value !== 'playlist'; renderCommand();
    };
    const requestEmergency = () => document.dispatchEvent(new CustomEvent('radios:emergency-draft-request', { detail: { strategy: strategy.value, name: text(name.value, 100) } }));
    strategy.addEventListener('change', () => { const before = strategy.dataset.emBefore || ''; requestEmergency(); $('[data-em-name-field]').hidden = strategy.value !== 'playlist'; record(`Devralma davranışı: ${before || '—'} → ${strategy.selectedOptions[0].textContent}`); strategy.dataset.emBefore = strategy.selectedOptions[0].textContent; });
    name.addEventListener('change', requestEmergency);
    name.addEventListener('input', renderCommand);
    document.addEventListener('radios:emergency-draft-change', (event) => { syncEmergency(event.detail); strategy.dataset.emBefore = strategy.selectedOptions[0]?.textContent || ''; });
    sourceSelect.addEventListener('change', () => { $('[data-emergency-add-source]').disabled = !sources.get(sourceSelect.value)?.items.length; });
    playerSelect.addEventListener('focus', () => { refreshPlayers(); renderCommand(); });
    $('[data-emergency-preview-volume]').addEventListener('input', (event) => { previewGain = Math.max(-60, Math.min(6, Number(event.target.value))); renderGain(); persist(); });
    form.addEventListener('input', (event) => { if (['duration', ...MARKS].includes(event.target.name)) { event.target.setCustomValidity(''); renderMarkers(); } if (event.target.name === 'title') event.target.setCustomValidity(''); });
    form.addEventListener('submit', (event) => {
        event.preventDefault();
        if (!text(form.elements.title.value)) form.elements.title.setCustomValidity('İçerik adı gir.');
        validateMarks();
        if (!form.reportValidity()) return;
        const loaded = audioFiles.get(fileKey());
        const values = { ...Object.fromEntries(new FormData(form)), duration: formDuration(), source: loaded ? 'file' : 'manual', fileName: loaded?.name || selectedItem()?.fileName || '' };
        for (const mark of MARKS) values[mark] = parseTime(form.elements[mark].value);
        const cleaned = cleanItem(values); if (!cleaned) return;
        const item = selectedItem();
        if (item) Object.assign(item, cleaned);
        else { const entry = { ...cleaned, id: uid('emergency-item') }; items.push(entry); selectedId = entry.id; if (loaded) { audioFiles.set(entry.id, loaded); audioFiles.delete('draft'); } }
        record(`${cleaned.title} · içerik ve cue kaydedildi.`); renderList(); fillItem(); renderChips();
    });

    /* Kısa listeler: planlı sessizlik, duyuru, anahtar, iletişim, tatbikat */
    const miniRow = (main, meta, actions = [], off = false) => {
        const row = node('li', off ? 'is-off' : ''); const lead = node('span'); lead.append(...(Array.isArray(main) ? main : [main]));
        row.append(lead); if (meta) row.append(node('small', '', meta));
        for (const [caption, data, extra] of actions) { const button = node('button', '', caption); button.type = 'button'; Object.assign(button.dataset, data); if (extra) button.setAttribute('aria-label', extra); row.append(button); }
        return row;
    };
    const renderQuiet = () => {
        quietWindows.sort((a, b) => a.start.localeCompare(b.start));
        $('[data-em-quiet-list]').replaceChildren(...quietWindows.map((entry) => miniRow(entry.reason, `${clock(entry.start)} · ${entry.minutes} dk`, [['×', { emQuietRemove: entry.id }, `${entry.reason} penceresini kaldır`]], Date.parse(`${entry.start}Z`) + entry.minutes * 60000 < Date.now())));
    };
    const renderAlerts = () => {
        $('[data-em-alert-list]').replaceChildren(...alerts.map((alert) => {
            const tag = leveled(node('span', 'em-tag', alert.purpose === 'actual' ? 'GERÇEK' : 'TEST'), alert.purpose === 'actual' ? 'warn' : '');
            const summary = text(alert.text, 70) || alert.media;
            const meta = [`${clock(alert.startsAt)} – ${clock(alert.endsAt)}`, alert.outputs.map((output) => OUTPUTS[output]).join(', '), alert.repeatMinutes ? `${alert.repeatMinutes} dk tekrar` : '', alert.geoCodes.join(' '), alert.status === 'cancelled' ? `İptal · ${CANCEL[alert.cancelReason]}` : 'Taslak'].filter(Boolean).join(' · ');
            const row = miniRow([tag, ` ${summary}`], meta, alert.status === 'draft' ? [['Düzenle', { emAlertEdit: alert.id }], ['İptal et', { emAlertCancel: alert.id }]] : [], alert.status === 'cancelled');
            if (row.dataset.cancelling === 'true') return row;
            return row;
        }));
        renderChips();
    };
    const alertForm = $('[data-em-alert-form]');
    const resetAlertForm = () => {
        alertForm.reset(); delete alertForm.dataset.editing;
        const start = new Date(Math.ceil(Date.now() / 300000) * 300000);
        alertForm.elements.startsAt.value = utcMinute(start); alertForm.elements.endsAt.value = utcMinute(new Date(start.getTime() + 1800000));
        setText('[data-em-alert-save]', 'Taslağı kaydet');
    };
    alertForm.addEventListener('input', (event) => event.target.setCustomValidity?.(''));
    alertForm.addEventListener('submit', (event) => {
        event.preventDefault();
        const data = new FormData(alertForm);
        const values = { id: alertForm.dataset.editing, purpose: data.get('purpose'), startsAt: data.get('startsAt'), endsAt: data.get('endsAt'), geoCodes: data.get('geoCodes'), outputs: data.getAll('outputs'), repeatMinutes: data.get('repeatMinutes'), text: data.get('text'), language: data.get('language'), media: data.get('media'), status: 'draft' };
        alertForm.elements.endsAt.setCustomValidity(values.endsAt && values.startsAt && values.endsAt <= values.startsAt ? 'Bitiş başlangıçtan sonra olmalı.' : '');
        alertForm.elements.text.setCustomValidity(!text(values.text) && !text(values.media) ? 'Metin veya ses dosyası gir.' : '');
        alertForm.querySelector('[name="outputs"]').setCustomValidity(values.outputs.length ? '' : 'En az bir çıkış seç.');
        const invalidCodes = text(values.geoCodes, 300).split(/[\s,;]+/).filter((code) => code && !CODE.test(code));
        alertForm.elements.geoCodes.setCustomValidity(invalidCodes.length ? `Geçersiz kod: ${invalidCodes[0]}` : '');
        if (!alertForm.reportValidity()) return;
        const alert = cleanAlert(values); if (!alert) return;
        const index = alerts.findIndex((entry) => entry.id === alert.id);
        if (index >= 0) alerts[index] = alert; else alerts.unshift(alert);
        alerts.splice(50);
        record(`${alert.purpose === 'actual' ? 'Gerçek' : 'Test'} acil duyuru taslağı ${index >= 0 ? 'güncellendi' : 'kaydedildi'}: ${clock(alert.startsAt)}–${clock(alert.endsAt)}.`);
        resetAlertForm(); renderAlerts();
    });
    const renderKeys = () => $('[data-em-key-list]').replaceChildren(...keys.map((entry) => miniRow(entry.label, `${entry.owner} · ${clock(entry.createdAt)} · ${entry.status === 'revoked' ? 'İptal' : 'Kayıtlı'}`, entry.status === 'registered' ? [['İptal et', { emKeyRevoke: entry.id }, `${entry.label} anahtarını iptal et`]] : [], entry.status === 'revoked')));
    const renderContacts = () => {
        $('[data-em-contact-list]').replaceChildren(...contacts.map((entry, index) => miniRow(`${index + 1}. ${entry.name}`, [entry.role, entry.phone, entry.offHours ? 'Mesai dışı' : ''].filter(Boolean).join(' · '), [...(index ? [['↑', { emContactUp: entry.id }, `${entry.name} yukarı`]] : []), ['×', { emContactRemove: entry.id }, `${entry.name} kaldır`]])));
        setText('[data-em-contacts-count]', `${contacts.length} kişi`);
    };
    const renderDrill = () => {
        const drill = drills[0];
        if (!drill) { facts('[data-em-drill-last]', [['Son tatbikat', '', ''], ['Algıla / karar / devral / topar', '', ''], ['Toplam', '', ''], ['Boşluk · çift ses · sonuç', '', '']]); renderChips(); return; }
        const total = drill.detect + drill.decide + drill.takeover + drill.recover;
        const over = settings.rto !== '' && total > settings.rto;
        facts('[data-em-drill-last]', [
            ['Son tatbikat', `${clock(drill.at)} · ${SCENARIOS[drill.scenario]}`, ''],
            ['Algıla / karar / devral / topar', `${drill.detect} / ${drill.decide} / ${drill.takeover} / ${drill.recover} sn`, ''],
            ['Toplam', `${total} sn${settings.rto !== '' ? ` · RTO ${settings.rto} sn` : ''}`, over ? 'crit' : settings.rto !== '' ? 'ok' : ''],
            ['Boşluk · çift ses · sonuç', `${drill.gapMs} ms · ${drill.double === 'yes' ? 'var' : 'yok'} · ${RESULTS[drill.result][0]}`, drill.double === 'yes' ? 'crit' : RESULTS[drill.result][1]],
        ]);
        renderChips();
    };
    const inlineForm = (selector, handler) => { const target = $(selector); target.addEventListener('input', (event) => event.target.setCustomValidity?.('')); target.addEventListener('submit', (event) => { event.preventDefault(); if (!target.reportValidity()) return; if (handler(new FormData(target), target) !== false) target.reset(); }); return target; };
    inlineForm('[data-em-quiet-form]', (data, target) => {
        const entry = { id: uid('quiet'), start: minute(data.get('start')), minutes: integer(data.get('minutes'), 1, 240), reason: text(data.get('reason'), 80) };
        if (!entry.start || entry.minutes === '' || !entry.reason) { target.elements.start.setCustomValidity('Geçerli bir pencere gir.'); target.reportValidity(); return false; }
        quietWindows.push(entry); quietWindows.splice(50); record(`Planlı sessizlik: ${clock(entry.start)} · ${entry.minutes} dk · ${entry.reason}`); renderQuiet();
    });
    inlineForm('[data-em-key-form]', (data) => {
        const entry = { id: uid('key'), label: text(data.get('label'), 60), owner: text(data.get('owner'), 80), createdAt: new Date().toISOString(), status: 'registered' };
        keys.unshift(entry); keys.splice(50); record(`USB anahtar kaydı eklendi: ${entry.label} · ${entry.owner}`); renderKeys();
    });
    inlineForm('[data-em-contact-form]', (data) => {
        contacts.push({ id: uid('contact'), name: text(data.get('name'), 80), role: text(data.get('role'), 60), phone: text(data.get('phone'), 30), offHours: data.get('offHours') === 'on' }); contacts.splice(30);
        record(`İletişim zincirine eklendi: ${text(data.get('name'), 80)}`); renderContacts();
    });
    const drillForm = inlineForm('[data-em-drill-form]', (data) => {
        const drill = cleanDrill({ ...Object.fromEntries(data), id: uid('drill') }); if (!drill) return false;
        drills.unshift(drill); drills.sort((a, b) => b.at.localeCompare(a.at)); drills.splice(20);
        record(`Tatbikat kaydedildi: ${SCENARIOS[drill.scenario]} · ${RESULTS[drill.result][0]} · boşluk ${drill.gapMs} ms`); renderDrill();
    });
    drillForm.addEventListener('reset', () => window.setTimeout(() => { drillForm.elements.at.value = utcMinute(); }));

    workspace.addEventListener('click', (event) => {
        const button = event.target instanceof Element ? event.target.closest('button') : null; if (!button || button.disabled) return;
        const data = button.dataset;
        if (data.emGoto) goto(data.emGoto, button);
        else if (data.emTab) showSide(data.emTab);
        else if (data.emLeftTab) showLeft(data.emLeftTab);
        else if (button.matches('[data-em-return]')) arm(button, 'return');
        else if (button.matches('[data-em-abort]')) arm(button, 'abort');
        else if (button.matches('[data-emergency-add-source]')) { const source = sources.get(sourceSelect.value); if (source) addItems(source.items, source.name); }
        else if (button.matches('[data-emergency-add-selected]')) addItems([window.RadioSProgramme?.getSnapshot()?.selectedItem]);
        else if (data.emergencySelect) { if (selectedId !== data.emergencySelect) stopPreview(); selectedId = data.emergencySelect; persist(); renderList(); fillItem(); }
        else if (button.matches('[data-emergency-new-item]')) { stopPreview(); selectedId = null; audioFiles.delete('draft'); renderList(); fillItem(); form.elements.title.focus(); }
        else if (data.emergencyRowCue) { if (playingId === data.emergencyRowCue) stopPreview(); else { if (selectedId !== data.emergencyRowCue) { selectedId = data.emergencyRowCue; persist(); renderList(); fillItem(); } startPreview(data.emergencyRowCue); } }
        else if (button.matches('[data-emergency-cue]')) { if (playingId === fileKey()) stopPreview(); else startPreview(fileKey()); }
        else if (data.emergencyRemove) removeItem(data.emergencyRemove);
        else if (data.emergencyListAction) {
            if (data.emergencyListAction === 'remove') { removeItem(selectedId); return; }
            const index = items.findIndex((item) => item.id === selectedId); const next = index + (data.emergencyListAction === 'up' ? -1 : 1);
            if (index < 0 || next < 0 || next >= items.length) return;
            const [item] = items.splice(index, 1); items.splice(next, 0, item); record(`${item.title} acil listede ${next + 1}. sıraya taşındı.`); renderList();
        }
        else if (data.emAlarm) { alarmNotes[data.emAlarm] = data.emAlarmAction; record(`${data.emAlarmLayer} alarmı ${data.emAlarmAction === 'muted' ? 'susturuldu' : 'kabul edildi'}.`); renderChain(); }
        else if (data.emQuietRemove) { const index = quietWindows.findIndex((entry) => entry.id === data.emQuietRemove); if (index >= 0) { const [removed] = quietWindows.splice(index, 1); record(`Planlı sessizlik kaldırıldı: ${removed.reason}`); renderQuiet(); } }
        else if (data.emAlertEdit) {
            const alert = alerts.find((entry) => entry.id === data.emAlertEdit); if (!alert) return;
            alertForm.dataset.editing = alert.id;
            for (const field of ['startsAt', 'endsAt', 'repeatMinutes', 'text', 'media']) alertForm.elements[field].value = String(alert[field] ?? '');
            alertForm.elements.geoCodes.value = alert.geoCodes.join(', '); alertForm.elements.language.value = alert.language || 'tr-TR';
            alertForm.querySelectorAll('[name="purpose"]').forEach((radio) => { radio.checked = radio.value === alert.purpose; });
            alertForm.querySelectorAll('[name="outputs"]').forEach((box) => { box.checked = alert.outputs.includes(box.value); });
            setText('[data-em-alert-save]', 'Taslağı güncelle'); alertForm.elements.startsAt.focus();
        }
        else if (data.emAlertCancel) {
            const row = button.closest('li'); if (row.querySelector('select')) return;
            const reason = node('select'); reason.setAttribute('aria-label', 'İptal gerekçesi');
            for (const [value, caption] of Object.entries(CANCEL)) { const option = node('option', '', caption); option.value = value; reason.append(option); }
            const confirm = node('button', '', 'İptali onayla'); confirm.type = 'button'; confirm.dataset.emAlertConfirm = data.emAlertCancel;
            button.replaceWith(reason, confirm); reason.focus();
        }
        else if (data.emAlertConfirm) {
            const alert = alerts.find((entry) => entry.id === data.emAlertConfirm); const reason = button.previousElementSibling?.value;
            if (!alert || !Object.hasOwn(CANCEL, reason)) return;
            alert.status = 'cancelled'; alert.cancelReason = reason; if (alertForm.dataset.editing === alert.id) resetAlertForm();
            record(`Acil duyuru iptal edildi: ${CANCEL[reason]}`); renderAlerts();
        }
        else if (data.emKeyRevoke) { const entry = keys.find((key) => key.id === data.emKeyRevoke); if (entry) { entry.status = 'revoked'; record(`USB anahtarı iptal edildi: ${entry.label}`); renderKeys(); } }
        else if (data.emContactUp) { const index = contacts.findIndex((entry) => entry.id === data.emContactUp); if (index > 0) { [contacts[index - 1], contacts[index]] = [contacts[index], contacts[index - 1]]; record(`İletişim sırası: ${contacts[index - 1].name} ${index}. sıraya çıktı.`); renderContacts(); } }
        else if (data.emContactRemove) { const index = contacts.findIndex((entry) => entry.id === data.emContactRemove); if (index >= 0) { const [removed] = contacts.splice(index, 1); record(`İletişim zincirinden çıkarıldı: ${removed.name}`); renderContacts(); } }
        else if (button.matches('[data-em-alert-new]')) resetAlertForm();
        else if (button.matches('[data-em-handover]')) {
            const missing = [['responsible', 'Devreden'], ['receiver', 'Devralan'], ['lastState', 'Son durum']].find(([field]) => !settings[field]);
            if (missing) { const input = $(`[data-em-setting="${missing[0]}"]`); input.setCustomValidity(`${missing[1]} gerekli.`); input.reportValidity(); return; }
            record(`Devir: ${settings.responsible} → ${settings.receiver} · Son durum: ${settings.lastState}${settings.actions ? ` · Yapılan: ${settings.actions}` : ''}${settings.risk ? ` · Risk: ${settings.risk}` : ''}`);
        }
        else if (button.matches('[data-emergency-save]')) {
            const invalid = $$('[data-em-setting]').find((input) => !input.checkValidity());
            const silence = settings.silenceEnabled ? ['silenceDb', 'silenceSeconds'].find((field) => settings[field] === '') : null;
            const restore = settings.restoreDb !== '' && settings.silenceDb !== '' && settings.restoreDb <= settings.silenceDb;
            const problem = invalid || (silence && $(`[data-em-setting="${silence}"]`)) || (restore && $('[data-em-setting="restoreDb"]'));
            if (problem) {
                if (silence && problem.dataset.emSetting === silence) problem.setCustomValidity('Otomatik devralma için gerekli.');
                if (restore && problem.dataset.emSetting === 'restoreDb') problem.setCustomValidity('Dönüş eşiği sessizlik eşiğinden yüksek olmalı.');
                const panel = problem.closest('[data-em-panel]'); if (panel) showSide(panel.dataset.emPanel);
                problem.reportValidity(); problem.focus(); return;
            }
            requestEmergency(); if (persist()) record('Acil hazırlık kaydedildi.');
        }
        else if (button.matches('[data-emergency-download]')) {
            const exported = { type: 'RadioS acil hazırlık kaydı', exportedAt: new Date().toISOString(), ...snapshot(), emergency: { strategy: strategy.value, name: text(name.value, 100) }, actualBroadcastState: status ? { state: takeoverState(), observedAt: new Date(statusAt).toISOString(), stale: !fresh() } : 'Bilinmiyor' };
            const url = URL.createObjectURL(new Blob([JSON.stringify(exported, null, 2)], { type: 'application/json' })); const link = node('a'); link.href = url; link.download = 'radios-acil-hazirlik.json'; document.body.append(link); link.click(); link.remove(); window.setTimeout(() => URL.revokeObjectURL(url), 1000);
            record('Acil hazırlık kaydı indirildi.');
        }
    });
    $$('[data-em-setting="responsible"], [data-em-setting="receiver"], [data-em-setting="lastState"]').forEach((input) => input.addEventListener('input', () => input.setCustomValidity('')));

    /* Sürükle bırak: acil liste ve sekmeler arasında yaylı geçiş */
    const accepts = (event) => Array.from(event.dataTransfer?.types || []).includes(mime);
    for (const eventName of ['dragenter', 'dragover']) listPanel.addEventListener(eventName, (event) => { if (!accepts(event)) return; event.preventDefault(); event.dataTransfer.dropEffect = 'copy'; listPanel.classList.add('is-drop-target'); });
    listPanel.addEventListener('dragleave', (event) => { if (!listPanel.contains(event.relatedTarget)) listPanel.classList.remove('is-drop-target'); });
    listPanel.addEventListener('drop', (event) => {
        listPanel.classList.remove('is-drop-target'); if (!accepts(event)) return; event.preventDefault();
        try { const payload = JSON.parse(event.dataTransfer.getData(mime)); addItems(payload.items, payload.playlistName); } catch { live('Aktarım okunamadı.'); }
    });
    let springTimer = null;
    workspace.addEventListener('dragover', (event) => {
        const tab = event.target.closest?.('[data-em-left-tab]'); if (!tab || !accepts(event)) return;
        event.preventDefault(); if (tab.getAttribute('aria-selected') === 'true' || springTimer) return;
        springTimer = window.setTimeout(() => { springTimer = null; showLeft(tab.dataset.emLeftTab); }, 450);
    });
    workspace.addEventListener('dragleave', (event) => { if (event.target.closest?.('[data-em-left-tab]')) { window.clearTimeout(springTimer); springTimer = null; } });
    document.addEventListener('dragend', () => { listPanel.classList.remove('is-drop-target'); window.clearTimeout(springTimer); springTimer = null; });

    const queueCount = $('[data-queue-count]');
    const syncQueueCount = () => setText('[data-em-queue-count]', String(Number.parseInt(queueCount?.textContent || '0', 10) || 0));
    if (queueCount) new MutationObserver(syncQueueCount).observe(queueCount, { childList: true, characterData: true, subtree: true });

    /* Motor/SDBASE bağlantısı için tek giriş; çağrılmadıkça değerler “—” kalır (UZAK-04). */
    const applyStatus = (input) => {
        if (!input || typeof input !== 'object') return false;
        status = structuredClone(input); statusAt = instant(input.observedAt) ? Math.min(Date.now(), Date.parse(input.observedAt)) : Date.now();
        renderStatus(); return true;
    };
    document.addEventListener('radios:emergency-status', (event) => applyStatus(event.detail));
    window.RadioSEmergencyWorkspace = Object.freeze({ snapshot, applyStatus });
    // The protected takeover dialog (operations-ui.js) shows the same readiness values as this tab.
    const dialog = document.getElementById('emergency-dialog');
    $('[data-open-emergency]').addEventListener('click', () => {
        if (!dialog) return;
        const own = status?.ownership || {}; const cover = coverage(); const drill = drills[0];
        const values = { outputs: settings.outputs.map((output) => OUTPUTS[output]).join(', '), engines: own.activeEngine || own.standbyEngine ? `${text(own.activeEngine, 40) || '—'} / ${text(own.standbyEngine, 40) || '—'}` : '', verified: items.length && cover.checked ? `${cover.verified} / ${items.length}` : '', coverage: items.length ? formatSpan(cover.known) : '', drill: drill ? `${clock(drill.at)} · ${RESULTS[drill.result][0]}` : '' };
        for (const [field, value] of Object.entries(values)) { const target = dialog.querySelector(`[data-em-dialog="${field}"]`); if (target) target.textContent = value || '—'; }
    });
    banner?.querySelector('[data-em-global-open]')?.addEventListener('click', () => document.getElementById('profile-emergency-tab')?.click());
    backupSummary?.closest('button')?.addEventListener('click', () => document.getElementById('profile-emergency-tab')?.click());

    for (const eventName of ['radios:programme-change', 'radios:playlist-change', 'radios:live-player-change']) document.addEventListener(eventName, refreshSources);
    document.addEventListener('radios:profile-change', (event) => { stopPreview(); refreshSources(); renderList(); if (event.detail?.profile === 'emergency') window.requestAnimationFrame(drawWave); });
    document.addEventListener('visibilitychange', () => { if (document.hidden) stopPreview(); });
    window.addEventListener('pagehide', () => { stopPreview(); audioContext?.close().catch(() => {}); });
    window.setInterval(() => { if (status) { renderCommand(); renderChips(); } if (!workspace.hidden && items.length) renderCoverage(); }, 5000);

    writeSettings(); activateTabs('data-em-tab', view.side); activateTabs('data-em-left-tab', view.left);
    refreshSources(); syncEmergency(window.RadioSOperations?.emergencySnapshot?.()); strategy.dataset.emBefore = strategy.selectedOptions[0]?.textContent || '';
    resetAlertForm(); drillForm.elements.at.value = utcMinute();
    renderList(); fillItem(); renderAudit(); renderQuiet(); renderAlerts(); renderKeys(); renderContacts(); renderDrill(); renderStatus(); syncQueueCount();
})();
