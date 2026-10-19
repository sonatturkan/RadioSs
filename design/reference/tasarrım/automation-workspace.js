(() => {
    'use strict';
    const workspace = document.querySelector('[data-automation-workspace]');
    const form = workspace?.querySelector('[data-automation-form]');
    if (!form) return;
    const q = (selector) => workspace.querySelector(selector);
    const qa = (selector) => Array.from(workspace.querySelectorAll(selector));
    const field = (name) => form.elements.namedItem(name);
    const storageKey = 'radios.automation-rules.v1';
    const copy = (value) => JSON.parse(JSON.stringify(value));
    const clean = (value, max = 160) => typeof value === 'string' ? value.trim().slice(0, max) : '';
    const normalize = (value) => clean(String(value || '')).toLocaleLowerCase('tr-TR').normalize('NFD').replace(/\p{M}/gu, '').replace(/ı/g, 'i');
    const bounded = (value, fallback, min, max) => value !== '' && value !== null && Number.isFinite(Number(value)) ? Math.max(min, Math.min(max, Number(value))) : fallback;
    const integer = (value, fallback, min, max) => Math.round(bounded(value, fallback, min, max));
    const validTime = (value) => typeof value === 'string' && /^(?:[01][0-9]|2[0-3]):[0-5][0-9]$/.test(value);
    const validDate = (value) => typeof value === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(value) && Number.isFinite(Date.parse(`${value}T00:00:00Z`)) && new Date(`${value}T00:00:00Z`).toISOString().slice(0, 10) === value;
    const today = new Date().toISOString().slice(0, 10);
    const rawDuration = (track) => /^[0-9]{1,3}:[0-5][0-9]$/.test(track?.duration || '') ? Number(track.duration.split(':')[0]) * 60 + Number(track.duration.split(':')[1]) : 0;
    const duration = (track) => {
        const raw = rawDuration(track); if (!raw || !track.markers) return raw;
        const cueIn = bounded(track.markers.cueIn,0,0,raw);
        const cueOut = bounded(track.markers.out ?? track.markers.cueOut,raw,cueIn,raw);
        return Math.max(0,cueOut - cueIn);
    };
    const formatSeconds = (value) => `${Math.floor(Math.round(value) / 60)}:${String(Math.round(value) % 60).padStart(2, '0')}`;
    const enumValue = (value, choices, fallback) => choices.includes(value) ? value : fallback;
    const defaults = { name:'Genel rotasyon', sources:[], sourceMode:'cycle', trackMode:'random', trackGap:60, artistGap:30, albumGap:0, includeProgramme:true, jingleEnabled:false, jinglePlaylist:'', jingleEvery:3, planDate:today, startTime:'08:00', previewCount:20, windowStart:'00:00', windowEnd:'00:00', weekdays:[1,2,3,4,5,6,7], transition:'end', overlap:3, boundaryTransition:'same', boundaryOverlap:3, jingleDry:true, seed:'radio-s-1', deckMode:'alternate', decks:[], jingleDeck:'', fallbackMode:'stop', fallbackPlaylist:'', requireMetadata:false };
    const normalizeSource = (source) => ({ playlistId:clean(source?.playlistId, 120), weight:integer(source?.weight, 1, 1, 100), count:integer(source?.count, 1, 1, 100), start:validTime(source?.start) ? source.start : '00:00', end:validTime(source?.end) ? source.end : '00:00', days:enumValue(source?.days, ['all','weekdays','weekend'], 'all') });
    const normalizeRules = (input) => {
        const source = input && typeof input === 'object' ? input : {};
        const result = { ...copy(defaults) };
        result.name = clean(source.name, 80) || defaults.name;
        result.sources = Array.isArray(source.sources) ? source.sources.slice(0, 50).filter((entry) => entry && typeof entry === 'object').map(normalizeSource) : [];
        for (const name of ['trackGap','artistGap','albumGap']) result[name] = integer(source[name], defaults[name], 0, 10080);
        for (const name of ['includeProgramme','jingleEnabled','jingleDry','requireMetadata']) if (typeof source[name] === 'boolean') result[name] = source[name];
        for (const name of ['jinglePlaylist','fallbackPlaylist','jingleDeck']) result[name] = clean(source[name], 120);
        for (const name of ['startTime','windowStart','windowEnd']) if (validTime(source[name])) result[name] = source[name];
        result.planDate = validDate(source.planDate) ? source.planDate : today;
        result.jingleEvery = integer(source.jingleEvery, 3, 1, 100); result.previewCount = integer(source.previewCount, 20, 1, 200);
        result.overlap = bounded(source.overlap, 3, 0, 30); result.boundaryOverlap = bounded(source.boundaryOverlap, 3, 0, 30); result.seed = clean(source.seed, 64) || defaults.seed;
        result.weekdays = Array.isArray(source.weekdays) ? [...new Set(source.weekdays.filter((day) => Number.isInteger(day) && day >= 1 && day <= 7))] : copy(defaults.weekdays);
        result.decks = Array.isArray(source.decks) ? [...new Set(source.decks.filter((id) => typeof id === 'string').map((id) => clean(id, 120)))].slice(0, 50) : [];
        for (const [name, options] of Object.entries({sourceMode:['cycle','weighted','complete'],trackMode:['random','ordered','least-used'],transition:['end','crossfade','markers','manual'],boundaryTransition:['same','end','crossfade','markers','manual'],deckMode:['alternate','fixed'],fallbackMode:['stop','next','playlist']})) result[name] = enumValue(source[name], options, defaults[name]);
        return result;
    };
    let rules = normalizeRules(null), savedAt = '', restored = false, plan = null, applying = false;
    let library = [], players = [], sourceFingerprint = '', playerFingerprint = '';
    try { const saved = JSON.parse(localStorage.getItem(storageKey) || 'null'); if (saved?.rules) { rules = normalizeRules(saved.rules); savedAt = clean(saved.savedAt, 40); restored = true; } } catch (_) { /* A damaged local draft must not prevent planning. */ }
    const element = (tag, className, content) => { const node = document.createElement(tag); if (className) node.className = className; if (content !== undefined) node.textContent = content; return node; };
    const button = (label, attribute, value = '') => { const node = element('button', '', label); node.type = 'button'; node.setAttribute(attribute, value); return node; };
    const setText = (selector, value) => { const node = q(selector); if (node) node.textContent = value; };
    const announce = (message, warning = false) => { setText('[data-automation-status]', message); q('[data-automation-status]').classList.toggle('has-warning', warning); };
    const playlistById = (id) => library.find((playlist) => playlist.id === id);
    const playerById = (id) => players.find((player) => player.id === id);
    const readRules = () => {
        const result = { sources:copy(rules.sources) };
        for (const name of Object.keys(defaults)) {
            if (['sources','decks','weekdays'].includes(name)) continue;
            const input = field(name); if (input) result[name] = input.type === 'checkbox' ? input.checked : input.value;
        }
        result.weekdays = qa('[name="weekday"]:checked').map((input) => Number(input.value));
        result.decks = qa('[data-automation-deck]:checked').map((input) => input.value);
        return normalizeRules(result);
    };
    const fillForm = () => {
        for (const [name, value] of Object.entries(rules)) {
            if (['sources','decks','weekdays'].includes(name)) continue;
            const input = field(name); if (!input) continue;
            if (input.type === 'checkbox') input.checked = value; else input.value = String(value);
        }
        qa('[name="weekday"]').forEach((input) => { input.checked = rules.weekdays.includes(Number(input.value)); });
    };
    const fillPlaylistSelect = (select, selected, placeholder = 'Playlist seç') => {
        const blank = element('option', '', placeholder); blank.value = '';
        const options = [blank];
        library.forEach((playlist) => { const option = element('option', '', `${playlist.name} · ${playlist.items.length} parça`); option.value = playlist.id; options.push(option); });
        if (selected && !playlistById(selected)) { const missing = element('option', '', 'Kaldırılmış playlist · yeniden seç'); missing.value = selected; missing.disabled = true; options.push(missing); }
        select.replaceChildren(...options); select.value = selected;
    };
    const renderSources = () => {
        const rows = q('[data-automation-sources]'), fragment = document.createDocumentFragment();
        rules.sources.forEach((source, index) => {
            const row = element('tr'); row.dataset.automationSourceIndex = String(index);
            const sourceCell = element('td'), select = element('select'); select.dataset.automationSourceField = 'playlistId'; select.setAttribute('aria-label', `${index + 1}. kaynak playlist`);
            fillPlaylistSelect(select, source.playlistId); sourceCell.append(select);
            const details = element('details', 'automation-source-window'); details.append(element('summary', '', 'Gün / saat sınırı'));
            const controls = element('div', 'automation-source-window-fields');
            for (const [key, label] of [['start','Başlangıç'],['end','Bitiş']]) { const wrapper = element('label', '', label); const input = element('input'); input.type = 'time'; input.value = source[key]; input.dataset.automationSourceField = key; input.setAttribute('aria-label', `${index + 1}. kaynak ${label.toLocaleLowerCase('tr-TR')} saati UTC`); input.required = true; wrapper.append(input); controls.append(wrapper); }
            const dayLabel = element('label', '', 'Günler'), daySelect = element('select'); daySelect.dataset.automationSourceField = 'days';
            for (const [value, label] of [['all','Her gün'],['weekdays','Hafta içi'],['weekend','Hafta sonu']]) { const option = element('option', '', label); option.value = value; daySelect.append(option); } daySelect.value = source.days; dayLabel.append(daySelect); controls.append(dayLabel); details.append(controls); sourceCell.append(details); row.append(sourceCell);
            for (const key of ['weight','count']) { const cell = element('td'), input = element('input'); input.type = 'number'; input.min = '1'; input.max = '100'; input.step = '1'; input.required = true; input.value = String(source[key]); input.dataset.automationSourceField = key; input.setAttribute('aria-label', `${index + 1}. kaynak ${key === 'weight' ? 'ağırlığı' : 'turda parça sayısı'}`); cell.append(input); row.append(cell); }
            const actions = element('td'), group = element('div', 'automation-source-buttons');
            for (const [action, label, title] of [['up','↑','Kaynağı yukarı taşı'],['down','↓','Kaynağı aşağı taşı'],['remove','×','Kaynağı kaldır']]) { const control = button(label, 'data-automation-source-action', action); control.setAttribute('aria-label', `${index + 1}. ${title}`); control.disabled = action === 'up' && index === 0 || action === 'down' && index === rules.sources.length - 1; group.append(control); } actions.append(group); row.append(actions); fragment.append(row);
        });
        rows.replaceChildren(fragment); q('[data-automation-source-empty]').hidden = rules.sources.length > 0;
        q('[data-automation-source-add]').disabled = !library.length || rules.sources.length >= 50;
        qa('[data-automation-playlist-select]').forEach((select) => fillPlaylistSelect(select, rules[select.name]));
    };
    const renderDecks = () => {
        const container = q('[data-automation-decks]'), fragment = document.createDocumentFragment();
        players.forEach((player) => { const label = element('label'), input = element('input'); input.type = 'checkbox'; input.value = player.id; input.dataset.automationDeck = ''; input.checked = rules.decks.includes(player.id); label.append(input, document.createTextNode(player.name)); fragment.append(label); });
        if (!players.length) fragment.append(element('p', '', 'Yayın ekranında oynatıcı oluşturulmadı.'));
        container.replaceChildren(fragment);
        const select = field('jingleDeck'), blank = element('option', '', 'Müzik deck sırasını kullan'); blank.value = ''; select.replaceChildren(blank);
        players.forEach((player) => { const option = element('option', '', player.name); option.value = player.id; select.append(option); });
        if (rules.jingleDeck && !playerById(rules.jingleDeck)) { const missing = element('option', '', 'Kaldırılmış deck · yeniden seç'); missing.value = rules.jingleDeck; missing.disabled = true; select.append(missing); }
        select.value = rules.jingleDeck;
    };
    const renderSummary = () => {
        setText('[data-automation-summary-name]', rules.name);
        const selected = rules.sources.map((source) => playlistById(source.playlistId)).filter(Boolean);
        for (const [key, value] of Object.entries({sources:selected.length,tracks:selected.reduce((sum, playlist) => sum + playlist.items.length, 0),mode:{random:'Rastgele',ordered:'Playlist sırası','least-used':'En az planlanan'}[rules.trackMode],decks:rules.decks.map((id) => playerById(id)?.name).filter(Boolean).join(', ') || 'Atanmadı',gaps:`${rules.trackGap} / ${rules.artistGap} dk`,saved:savedAt ? new Date(savedAt).toLocaleString('tr-TR', {timeZone:'UTC', hour:'2-digit',minute:'2-digit',day:'2-digit',month:'2-digit'}) + ' UTC' : 'Kaydedilmedi'})) setText(`[data-automation-summary="${key}"]`, String(value));
        setText('[data-automation-selection-help]', {cycle:'Kaynak sırası ve turda parça adetleri uygulanır. Gün / saat dışında kalan kaynaklar o turda kullanılmaz.',weighted:'Ağırlık 5 olan playlist, ağırlık 1 olana göre yaklaşık beş kat sık seçilir. Adet sütunu bu yöntemde kullanılmaz.',complete:'Her playlistten, listedeki parça adedi kadar seçim yapıldıktan sonra sonraki kaynağa geçilir. Adet ve ağırlık bu yöntemde kullanılmaz.'}[rules.sourceMode]);
        field('overlap').disabled = ['end','manual'].includes(rules.transition);
        field('boundaryOverlap').disabled = !['crossfade','markers'].includes(rules.boundaryTransition);
        field('fallbackPlaylist').disabled = rules.fallbackMode !== 'playlist';
        field('jinglePlaylist').disabled = !rules.jingleEnabled; field('jingleEvery').disabled = !rules.jingleEnabled; field('jingleDeck').disabled = !rules.jingleEnabled;
    };
    const invalidate = (message = '') => {
        if (!plan || applying) return;
        plan.stale = true; q('[data-automation-apply]').disabled = true;
        setText('[data-automation-state]', 'Önizleme güncellenmeli');
        if (message) announce(message, true);
    };
    const refreshLibrary = () => {
        const snapshot = window.RadioSPlaylist?.snapshot?.() || window.RadioSPlaylists?.snapshot?.();
        const next = Array.isArray(snapshot?.playlists) ? snapshot.playlists.filter((playlist) => playlist && clean(playlist.id) && Array.isArray(playlist.items)).map((playlist) => ({...playlist,name:clean(playlist.name) || 'Adsız playlist'})) : [];
        const fingerprint = JSON.stringify(next); if (fingerprint === sourceFingerprint) return;
        library = next; sourceFingerprint = fingerprint; renderSources(); renderSummary();
        invalidate('Playlist içeriği değişti. Aktarmadan önce önizlemeyi yeniden oluştur.');
    };
    const refreshPlayers = (event) => {
        const exported = window.RadioSLivePlayers?.snapshot?.()?.players || event?.detail?.players;
        const next = Array.isArray(exported) ? exported.map((player) => ({id:clean(player.id, 120),name:clean(player.name) || 'Oynatıcı'})).filter((player) => player.id) : Array.from(document.querySelectorAll('[data-live-player]')).map((node) => ({id:node.dataset.livePlayer,name:node.querySelector('h3')?.textContent?.trim() || 'Oynatıcı'}));
        const fingerprint = JSON.stringify(next); if (fingerprint === playerFingerprint) return;
        players = next; playerFingerprint = fingerprint;
        if (!restored && !rules.decks.length && !plan) rules.decks = players.slice(0, 2).map((player) => player.id);
        renderDecks(); renderSummary(); invalidate('Oynatıcılar değişti. Deck atamalarını kontrol ederek önizlemeyi yenile.');
    };
    const timeMinute = (value) => Number(value.split(':')[0]) * 60 + Number(value.split(':')[1]);
    const inWindow = (at, start, end) => { const date = new Date(at), minute = date.getUTCHours() * 60 + date.getUTCMinutes(), first = timeMinute(start), last = timeMinute(end); return first === last || first < last && minute >= first && minute < last || first > last && (minute >= first || minute < last); };
    const weekday = (at) => new Date(at).getUTCDay() || 7;
    const sourceActive = (source, at) => inWindow(at, source.start, source.end) && (source.days === 'all' || source.days === 'weekdays' && weekday(at) <= 5 || source.days === 'weekend' && weekday(at) >= 6);
    const ruleActive = (config, at) => config.weekdays.includes(weekday(at)) && inWindow(at, config.windowStart, config.windowEnd);
    const recordingIdentity = (track) => {
        const location = clean(track.location || track.sourceLocation,4096);
        if (location) {
            let canonical = location.replace(/^file:\/\/localhost(?=\/)/i,'file://');
            try { canonical = decodeURIComponent(canonical); } catch (_) { /* Keep malformed escapes as a stable literal URI. */ }
            return `file:${canonical}`;
        }
        if (clean(track.sourceTrackId)) return `source:${track.sourceTrackId}`;
        return `label:${normalize(`${track.artist || ''}|${track.title || ''}`)}`;
    };
    const identities = (track) => ({recording:recordingIdentity(track),artist:normalize(track.artist),album:normalize(track.album)});
    const randomFor = (seed) => { let state = 2166136261; for (const char of seed) { state ^= char.charCodeAt(0); state = Math.imul(state,16777619); } return () => { state += 0x6D2B79F5; let value = state; value = Math.imul(value ^ value >>> 15,value | 1); value ^= value + Math.imul(value ^ value >>> 7,value | 61); return ((value ^ value >>> 14) >>> 0) / 4294967296; }; };
    const previousHistory = (config, start) => {
        if (!config.includeProgramme) return [];
        const items = window.RadioSProgramme?.getEditableSequence?.() || [];
        let at = start - items.reduce((sum, track) => sum + duration(track) * 1000, 0);
        return items.map((track) => { const entry = {...identities(track),at}; at += duration(track) * 1000; return entry; });
    };
    const rejectionLabels = {duration:'süre eksik / sıfır',metadata:'sanatçı bilgisi eksik',missing:'dosya eksik / öğe kilitli',track:'parça tekrar aralığı',artist:'sanatçı tekrar aralığı',album:'albüm tekrar aralığı'};
    const rejection = (track, config, at, history, jingle = false) => {
        if (!track || !clean(track.title) || duration(track) <= 0) return 'duration';
        if (['missing','invalid','locked'].includes(track.readiness) || track.primaryState === 'missing') return 'missing';
        if (!jingle && config.requireMetadata && !clean(track.artist)) return 'metadata';
        const identity = identities(track);
        for (const [key, gap, code] of [['recording',config.trackGap,'track'],['artist',jingle ? 0 : config.artistGap,'artist'],['album',jingle ? 0 : config.albumGap,'album']]) {
            if (!gap || !identity[key]) continue;
            if (history.some((entry) => entry[key] === identity[key] && at >= entry.at && at - entry.at < gap * 60000)) return code;
        }
        return '';
    };
    const transitionFor = (from, to, config) => from && from.playlistId !== to.playlistId && config.boundaryTransition !== 'same' ? config.boundaryTransition : config.transition;
    const overlapFor = (from, to, config) => {
        const mode = transitionFor(from,to,config);
        if (!from || ['end','manual'].includes(mode) || config.jingleDry && (from.type === 'jingle' || to.type === 'jingle')) return 0;
        let value = from.playlistId !== to.playlistId && config.boundaryTransition !== 'same' ? config.boundaryOverlap : config.overlap;
        if (mode === 'markers') {
            const markers = from.track.markers;
            if (Number.isFinite(markers?.overlap)) value = markers.overlap;
            else if (Number.isFinite(markers?.autoNext)) value = Math.max(0, (markers.out ?? rawDuration(from.track)) - markers.autoNext);
        }
        return Math.max(0, Math.min(value, duration(from.track), duration(to.track)));
    };
    const buildPlan = (config) => {
        const result = {rules:copy(config),entries:[],diagnostics:[],complete:false,stale:false,applied:false,sourceFingerprint,playerFingerprint,programmeRevision:window.RadioSProgramme?.getSnapshot?.()?.programme?.revision ?? null,createdAt:new Date().toISOString()};
        const start = Date.parse(`${config.planDate}T${config.startTime}:00Z`), rng = randomFor(config.seed), history = previousHistory(config, start), cursors = new Map(), completeUsed = new Map();
        const assignedDecks = config.decks.filter((id) => playerById(id));
        const sources = config.sources.map((source, index) => ({...source, playlist:playlistById(source.playlistId),index}));
        if (!sources.length) result.diagnostics.push('Rotasyona en az bir kaynak playlist ekle.');
        if (sources.some((source) => !source.playlist)) result.diagnostics.push('Kaynaklardan birinin playlisti bulunamadı; yeniden seç.');
        if (!assignedDecks.length) result.diagnostics.push('Önizleme için en az bir mevcut deck seç.');
        if (assignedDecks.length !== config.decks.length) result.diagnostics.push('Seçilen decklerden biri kaldırılmış; atamayı güncelle.');
        const needsOverlap = config.transition === 'markers' || config.transition === 'crossfade' && config.overlap > 0 || config.boundaryTransition === 'markers' || config.boundaryTransition === 'crossfade' && config.boundaryOverlap > 0;
        if (needsOverlap && (assignedDecks.length < 2 || config.deckMode === 'fixed')) result.diagnostics.push('Bindirme için en az iki decki sırayla kullan; tek deckte parça bitişi veya manuel geçiş seç.');
        if (config.jingleEnabled && !playlistById(config.jinglePlaylist)?.items.length) result.diagnostics.push('Jingle etkin: dolu bir jingle playlisti seç.');
        if (config.jingleDeck && config.jingleEnabled && !playerById(config.jingleDeck)) result.diagnostics.push('Seçilen jingle decki bulunamadı.');
        if (config.fallbackMode === 'playlist' && !playlistById(config.fallbackPlaylist)?.items.length) result.diagnostics.push('Yedek politikası için dolu bir yedek playlist seç.');
        if (!ruleActive(config,start)) result.diagnostics.push('Plan başlangıcı seçilen gün / saat aralığının dışında.');
        if (result.diagnostics.length) return result;
        let at = start, cycleIndex = 0, cycleUsed = 0, musicCount = 0, deckIndex = 0, fallbackCount = 0, missingMetadata = 0;
        const rejectedCounts = new Map();
        const pickTrack = (playlist, jingle = false, used = null) => {
            const previous = result.entries.at(-1), cursor = cursors.get(playlist.id) || 0;
            const eligible = playlist.items.flatMap((track, index) => {
                if (used?.has(index)) return [];
                const possible = {track,type:jingle ? 'jingle' : track.type || 'music',playlistId:playlist.id};
                const overlap = overlapFor(previous,possible,config), candidateAt = at - overlap * 1000;
                const reason = rejection(track,config,candidateAt,history,jingle);
                if (reason) { rejectedCounts.set(reason,(rejectedCounts.get(reason) || 0) + 1); return []; }
                return [{track,index,overlap,candidateAt}];
            });
            if (!eligible.length) return null;
            let chosen;
            if (config.trackMode === 'ordered' || jingle) chosen = eligible.sort((a,b) => (a.index - cursor + playlist.items.length) % playlist.items.length - (b.index - cursor + playlist.items.length) % playlist.items.length)[0];
            else if (config.trackMode === 'random') chosen = eligible[Math.floor(rng() * eligible.length)];
            else chosen = eligible.map((candidate) => { const key = identities(candidate.track).recording, past = history.filter((entry) => entry.recording === key); return {...candidate,uses:past.length,last:past.at(-1)?.at ?? -Infinity}; }).sort((a,b) => a.uses - b.uses || a.last - b.last || a.index - b.index)[0];
            cursors.set(playlist.id,(chosen.index + 1) % playlist.items.length); return chosen;
        };
        const append = (playlist, chosen, type, reason) => {
            const deckId = type === 'jingle' && config.jingleDeck ? config.jingleDeck : assignedDecks[config.deckMode === 'fixed' ? 0 : deckIndex++ % assignedDecks.length];
            if (chosen.overlap > 0 && result.entries.at(-1)?.deckId === deckId) { result.diagnostics.push('Ardışık bindirmeli öğeler aynı decke atandı; jingle deckini veya dağılımı değiştir.'); return false; }
            const entry = {track:copy(chosen.track),type,playlistId:playlist.id,playlistName:playlist.name,deckId,deckName:playerById(deckId)?.name || '',scheduledAt:new Date(chosen.candidateAt).toISOString(),overlap:chosen.overlap,transitionMode:transitionFor(result.entries.at(-1),{playlistId:playlist.id},config),reason};
            result.entries.push(entry); history.push({...identities(chosen.track),at:chosen.candidateAt});
            at = chosen.candidateAt + duration(chosen.track) * 1000;
            if (type !== 'jingle' && (!clean(chosen.track.artist) || !clean(chosen.track.album))) missingMetadata++;
            return true;
        };
        for (let step = 0; step < config.previewCount; step++) {
            if (!ruleActive(config,at)) { result.diagnostics.push(`${musicCount}. müzikten sonra gün / saat aralığı sona erdi; plan durdu.`); break; }
            const active = sources.filter((source) => sourceActive(source,at));
            if (!active.length) { result.diagnostics.push(`${new Date(at).toISOString().slice(11,19)} UTC için açık kaynak playlist yok; plan durdu.`); break; }
            let source;
            if (config.sourceMode === 'weighted') { const total = active.reduce((sum,row) => sum + row.weight,0); let choice = rng() * total; source = active.find((row) => (choice -= row.weight) < 0) || active.at(-1); }
            else {
                if (!active.some((row) => row.index === cycleIndex)) { cycleIndex = active.find((row) => row.index >= cycleIndex)?.index ?? active[0].index; cycleUsed = 0; }
                source = sources[cycleIndex];
                const limit = config.sourceMode === 'complete' ? Math.max(1,source.playlist.items.length) : source.count;
                if (cycleUsed >= limit) { const current = active.findIndex((row) => row.index === cycleIndex); source = active[(current + 1) % active.length]; cycleIndex = source.index; cycleUsed = 0; completeUsed.delete(source.playlist.id); }
                cycleUsed++;
            }
            let playlist = source.playlist, chosenSource = source, used = config.sourceMode === 'complete' ? completeUsed.get(playlist.id) || new Set() : null, chosen = pickTrack(playlist,false,used), usedFallback = false;
            if (!chosen && config.fallbackMode === 'next') { for (const alternative of active.filter((row) => row.index !== source.index)) { const candidate = pickTrack(alternative.playlist); if (candidate && sourceActive(alternative,candidate.candidateAt)) { chosen = candidate; playlist = alternative.playlist; chosenSource = alternative; usedFallback = true; break; } } }
            if (!chosen && config.fallbackMode === 'playlist') { playlist = playlistById(config.fallbackPlaylist); chosen = pickTrack(playlist); usedFallback = Boolean(chosen); }
            if (!chosen) { result.diagnostics.push(`“${source.playlist.name}” için kurallara uygun parça bulunamadı; ${musicCount} müzik parçasından sonra plan durdu.`); break; }
            if (!ruleActive(config,chosen.candidateAt) || (config.fallbackMode !== 'playlist' || !usedFallback) && !sourceActive(chosenSource,chosen.candidateAt)) { result.diagnostics.push('Bindirme sonrası parça başlangıcı kaynak / gün aralığının dışında; plan durdu.'); break; }
            const reason = usedFallback ? 'Yedek seçim · kurallar korundu' : {random:'Rastgele · tekrar koruması',ordered:'Playlist sırası · tekrar koruması','least-used':'En az planlanan · tekrar koruması'}[config.trackMode];
            if (!append(playlist,chosen,chosen.track.type || 'music',reason)) break;
            if (used && !usedFallback) { used.add(chosen.index); completeUsed.set(playlist.id,used); }
            musicCount++; if (usedFallback) fallbackCount++;
            if (config.jingleEnabled && musicCount % config.jingleEvery === 0) {
                const jingle = playlistById(config.jinglePlaylist), selected = pickTrack(jingle,true);
                if (!selected) { result.diagnostics.push(`${musicCount}. müzikten sonra jingle seçilemedi; tekrar / süre kuralları nedeniyle plan durdu.`); break; }
                if (!ruleActive(config,selected.candidateAt)) { result.diagnostics.push('Jingle başlangıcı gün / saat aralığının dışında; plan durdu.'); break; }
                if (!append(jingle,selected,'jingle',`${config.jingleEvery} müzikte bir jingle`)) break;
            }
        }
        result.complete = musicCount === config.previewCount && !result.diagnostics.length;
        if (rejectedCounts.size) result.diagnostics.push(`Seçim sırasında elenen adaylar: ${[...rejectedCounts].map(([reason,count]) => `${rejectionLabels[reason]} (${count})`).join(', ')}. Aynı aday birden fazla adımda değerlendirilmiş olabilir.`);
        if (missingMetadata) result.diagnostics.push(`${missingMetadata} öğede sanatçı veya albüm bilgisi eksik; ilgili tekrar kontrolü bu alanlarda uygulanamadı.`);
        if (fallbackCount) result.diagnostics.push(`${fallbackCount} seçim yedek politikasından geldi; tekrar kuralları korundu.`);
        result.musicCount = musicCount; result.totalSeconds = Math.max(0,(at - start) / 1000); result.startAt = new Date(start).toISOString(); result.endAt = new Date(at).toISOString();
        return result;
    };
    const renderPlan = () => {
        const entries = plan?.entries || [], rows = q('[data-automation-preview-rows]'), fragment = document.createDocumentFragment();
        entries.forEach((entry,index) => {
            const row = element('tr',entry.type === 'jingle' ? 'automation-row-jingle' : '');
            const time = element('td'); time.append(element('strong','',String(index + 1).padStart(2,'0')),element('small','',entry.scheduledAt.slice(11,19))); if (entry.scheduledAt.slice(0,10) !== plan.rules.planDate) time.append(element('small','',entry.scheduledAt.slice(0,10))); row.append(time);
            const track = element('td'); track.append(element('strong','',entry.track.title),element('small','',entry.type === 'jingle' ? 'Jingle / istasyon kimliği' : entry.track.artist || 'Sanatçı belirtilmedi')); row.append(track);
            row.append(element('td','',entry.playlistName),element('td','',entry.deckName));
            const timing = element('td'); timing.append(element('strong','',entry.track.duration),element('small','',`${entry.overlap.toLocaleString('tr-TR')} sn bindirme`)); row.append(timing,element('td','',entry.reason)); fragment.append(row);
        });
        rows.replaceChildren(fragment); q('[data-automation-preview-table]').hidden = !entries.length; q('[data-automation-preview-empty]').hidden = entries.length > 0;
        q('[data-automation-download-plan]').disabled = !entries.length; q('[data-automation-apply]').disabled = !entries.length || plan.stale || plan.applied || !window.RadioSProgramme?.getSnapshot;
        q('[data-automation-cancel]').disabled = !plan;
        const diagnostics = plan?.diagnostics || []; q('[data-automation-diagnostics]').hidden = !diagnostics.length;
        q('[data-automation-diagnostic-list]').replaceChildren(...diagnostics.map((message) => element('li','',message)));
        setText('[data-automation-preview-summary]', !plan ? 'Kuralları belirle ve önizleme oluştur.' : `${entries.length} öğe · ${plan.musicCount || 0}/${plan.rules.previewCount} müzik · ${formatSeconds(plan.totalSeconds || 0)} süre${plan.applied ? ' · program taslağına eklendi' : plan.complete ? '' : ' · eksik plan'}`);
        setText('[data-automation-state]', !plan ? 'Planlayıcı beklemede' : plan.applied ? 'Plan program taslağına eklendi' : entries.length ? plan.complete ? 'Plan önizlemesi hazır' : 'Kısmi plan · kontrolleri incele' : 'Plan oluşturulamadı');
        q('[data-automation-apply]').textContent = plan?.applied ? 'Program sırasına eklendi' : plan && !plan.complete && entries.length ? 'Kısmi planı program sırasına ekle' : 'Program sırasına ekle';
    };
    const validateForm = () => { if (form.checkValidity()) return true; const invalid = form.querySelector(':invalid'), pane = invalid?.closest('.automation-pane'); if (pane) activatePane(pane.id.replace('automation-','').replace('-pane','')); form.reportValidity(); return false; };
    const generatePreview = () => {
        if (!validateForm()) return null; rules = readRules(); renderSummary(); plan = buildPlan(rules); renderPlan();
        announce(plan.entries.length ? `${plan.entries.length} öğe planlandı.${plan.complete ? '' : ' Kısmi planı ve durma nedenini kontrol et.'}` : plan.diagnostics[0] || 'Plan oluşturulamadı.',!plan.complete);
        document.dispatchEvent(new CustomEvent('radios:automation-preview',{detail:copy(plan)})); return copy(plan);
    };
    const transitionDetail = () => ({mode:rules.transition === 'markers' ? 'cue' : rules.transition,overlap:rules.overlap});
    const emitTransition = () => document.dispatchEvent(new CustomEvent('radios:automation-transition-change',{detail:transitionDetail()}));
    const setTransition = (input) => {
        if (!input || !['end','crossfade','cue','manual'].includes(input.mode)) return false;
        rules = readRules(); rules.transition = input.mode === 'cue' ? 'markers' : input.mode;
        if (Number.isFinite(Number(input.overlap))) rules.overlap = bounded(input.overlap,rules.overlap,0,30);
        fillForm(); renderSummary(); invalidate('Geçiş kuralı değişti; önizlemeyi yeniden oluştur.');
        const at = new Date().toISOString();
        try { localStorage.setItem(storageKey,JSON.stringify({rules,savedAt:at})); savedAt = at; setText('[data-automation-save-badge]','Kurallar kaydedildi'); } catch (_) { setText('[data-automation-save-badge]','Kalıcı kayıt yapılamadı'); }
        renderSummary(); emitTransition(); return transitionDetail();
    };
    const applyPlan = () => {
        if (!plan?.entries.length || plan.stale || plan.applied) return;
        if (JSON.stringify(readRules()) !== JSON.stringify(plan.rules) || sourceFingerprint !== plan.sourceFingerprint || playerFingerprint !== plan.playerFingerprint) { invalidate('Kurallar veya kaynaklar değişti; önizlemeyi yeniden oluştur.'); return; }
        const before = window.RadioSProgramme?.getSnapshot?.(); if (!before) { announce('Program taslağına erişilemiyor.',true); return; }
        if (plan.rules.includeProgramme && before.programme.revision !== plan.programmeRevision) { invalidate('Program sırası değişti; tekrar kontrolü için önizlemeyi yeniden oluştur.'); return; }
        const token = `automation-${Date.now().toString(36)}`;
        const items = plan.entries.map((entry,index) => {
            const next = plan.entries[index + 1], outgoingMode = next?.transitionMode || plan.rules.transition;
            const markers = {...copy(entry.track.markers || {}),automatic:outgoingMode !== 'manual'};
            const cueOut = Number.isFinite(markers.out) ? markers.out : rawDuration(entry.track);
            if (outgoingMode === 'end') markers.autoNext = cueOut;
            else if (outgoingMode === 'crossfade') markers.autoNext = Math.max(markers.cueIn || 0,cueOut - (next?.overlap || 0));
            if (next) markers.overlap = next.overlap;
            return {...copy(entry.track),type:entry.type,time:entry.scheduledAt.slice(11,16),sourceId:entry.track.sourceId || entry.track.id,markers,notes:`Otomasyon: ${plan.rules.name} · ${entry.deckName} · ${entry.playlistName}`.slice(0,300),automation:{planId:token,ordinal:index,ruleName:plan.rules.name,seed:plan.rules.seed,deckId:entry.deckId,deckName:entry.deckName,sourcePlaylistId:entry.playlistId,sourcePlaylistName:entry.playlistName,scheduledAt:entry.scheduledAt,transition:entry.transitionMode,automaticTransition:outgoingMode !== 'manual',overlapSeconds:entry.overlap,selectionReason:entry.reason}};
        });
        const oldIds = new Set(before.playbackItems.map((item) => item.id));
        applying = true;
        try {
            document.dispatchEvent(new CustomEvent('radios:add-programme-items',{detail:{kind:'programme',mode:'append',programmeName:`Otomasyon · ${plan.rules.name}`,playlistName:plan.rules.name,sourceProgrammeId:token,notes:`Otomasyon önizlemesi · ${plan.rules.planDate} ${plan.rules.startTime} UTC`,items}}));
            const after = window.RadioSProgramme.getSnapshot(), inserted = after.playbackItems.filter((item) => !oldIds.has(item.id));
            if (inserted.length !== items.length || inserted.some((item,index) => item.title !== items[index].title)) { announce('Program sırasına aktarım doğrulanamadı; mevcut sırayı kontrol et.',true); return; }
            inserted.forEach((item,index) => { if (!index) return; const overlap = plan.entries[index].overlap; if (overlap > 0) document.dispatchEvent(new CustomEvent('radios:transition-saved',{detail:{fromId:inserted[index - 1].id,toId:item.id,overlap}})); });
            plan.applied = true; plan.appliedItemIds = inserted.map((item) => item.id); renderPlan();
            announce(`${items.length} öğe yeni bir program bloğuna eklendi. Deck atamaları ve bindirmeler planla birlikte saklandı; kalıcı program kaydı için Yayın sekmesindeki Kaydet'i kullan.`);
            document.dispatchEvent(new CustomEvent('radios:automation-applied',{detail:{planId:token,itemIds:copy(plan.appliedItemIds),ruleName:plan.rules.name}}));
        } finally { applying = false; }
    };
    const download = (value, suffix) => { const blob = new Blob([JSON.stringify(value,null,2)],{type:'application/json;charset=utf-8'}), url = URL.createObjectURL(blob), link = document.createElement('a'); link.href = url; link.download = `radio-s-${suffix}-${rules.planDate}.json`; document.body.append(link); link.click(); link.remove(); setTimeout(() => URL.revokeObjectURL(url),1000); };
    const activatePane = (name, focus = false) => {
        const selected = q(`[data-automation-pane="${name}"]`); if (!selected) return;
        qa('[data-automation-pane]').forEach((tab) => { const active = tab === selected; tab.setAttribute('aria-selected',String(active)); tab.tabIndex = active ? 0 : -1; document.getElementById(tab.getAttribute('aria-controls')).hidden = !active; });
        if (focus) selected.focus();
    };
    workspace.addEventListener('click',(event) => {
        const target = event.target instanceof Element ? event.target.closest('button') : null; if (!target || target.disabled) return;
        if (target.matches('[data-automation-pane]')) activatePane(target.dataset.automationPane);
        else if (target.matches('[data-automation-source-add]')) { rules = readRules(); const playlist = library.find((candidate) => !rules.sources.some((source) => source.playlistId === candidate.id)) || library[0]; if (!playlist) return; rules.sources.push(normalizeSource({playlistId:playlist.id})); renderSources(); renderSummary(); invalidate(); }
        else if (target.matches('[data-automation-source-action]')) { const index = Number(target.closest('[data-automation-source-index]').dataset.automationSourceIndex), action = target.dataset.automationSourceAction; rules = readRules(); if (action === 'remove') rules.sources.splice(index,1); else { const next = index + (action === 'up' ? -1 : 1); if (next >= 0 && next < rules.sources.length) [rules.sources[index],rules.sources[next]] = [rules.sources[next],rules.sources[index]]; } renderSources(); renderSummary(); invalidate(); }
        else if (target.matches('[data-automation-new-seed]')) { field('seed').value = `radio-s-${Date.now().toString(36)}`; rules = readRules(); invalidate(); announce('Yeni rastgelelik anahtarı seçildi. Önizlemeyi yeniden oluştur.'); }
        else if (target.matches('[data-automation-preview]')) generatePreview();
        else if (target.matches('[data-automation-cancel]')) { plan = null; renderPlan(); announce('Önizleme temizlendi.'); }
        else if (target.matches('[data-automation-apply]')) applyPlan();
        else if (target.matches('[data-automation-export]')) { rules = readRules(); download({schema:'radios-automation-rules-v1',rules},'otomasyon-kurallari'); }
        else if (target.matches('[data-automation-download-plan]') && plan) download({schema:'radios-automation-plan-v1',...copy(plan)},'otomasyon-plani');
    });
    form.addEventListener('input',(event) => {
        const input = event.target; if (!(input instanceof Element)) return;
        if (input.matches('[data-automation-source-field]')) { const index = Number(input.closest('[data-automation-source-index]').dataset.automationSourceIndex); if (rules.sources[index]) rules.sources[index][input.dataset.automationSourceField] = input.value; }
        rules = readRules(); renderSummary(); invalidate(); setText('[data-automation-save-badge]','Kaydedilmemiş değişiklik');
        if (input.matches('[name="transition"], [name="overlap"]')) emitTransition();
    });
    form.addEventListener('change',(event) => { if (event.target.matches('[data-automation-source-field="playlistId"]')) renderSources(); });
    form.addEventListener('submit',(event) => {
        event.preventDefault(); if (!validateForm()) return; rules = readRules(); const at = new Date().toISOString();
        try { localStorage.setItem(storageKey,JSON.stringify({rules,savedAt:at})); savedAt = at; setText('[data-automation-save-badge]','Kurallar kaydedildi'); announce('Otomasyon kuralları bu tarayıcıya kaydedildi.'); } catch (_) { setText('[data-automation-save-badge]','Kalıcı kayıt yapılamadı'); announce('Tarayıcıda kalıcı kayıt yapılamadı. Kuralları indirerek saklayabilirsin.',true); }
        renderSummary(); document.dispatchEvent(new CustomEvent('radios:automation-rules-change',{detail:copy(rules)}));
    });
    q('.automation-tabs').addEventListener('keydown',(event) => {
        if (!['ArrowLeft','ArrowRight','Home','End'].includes(event.key)) return; const tabs = qa('[data-automation-pane]'), index = tabs.indexOf(event.target); if (index < 0) return; event.preventDefault(); const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (index + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length; activatePane(tabs[next].dataset.automationPane,true);
    });
    document.addEventListener('radios:playlist-change',refreshLibrary);
    document.addEventListener('radios:live-player-change',refreshPlayers);
    document.addEventListener('radios:programme-change',() => { if (rules.includeProgramme) invalidate('Program sırası değişti; önizlemeyi yeniden oluştur.'); });
    document.addEventListener('radios:profile-change',(event) => { if (event.detail?.profile === 'automation') { refreshLibrary(); refreshPlayers(); } });
    window.RadioSAutomation = Object.freeze({snapshot:() => ({rules:copy(readRules()),plan:plan ? copy(plan) : null,playlists:copy(library),players:copy(players),savedAt}),generatePreview,setTransition});
    fillForm(); refreshLibrary(); refreshPlayers(); renderSources(); renderDecks(); renderSummary(); renderPlan();
    if (savedAt) setText('[data-automation-save-badge]','Kaydedilen kurallar');
    emitTransition();
})();
