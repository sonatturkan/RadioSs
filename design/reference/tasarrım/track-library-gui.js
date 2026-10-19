(() => {
    'use strict';
    const library = document.querySelector('#broadcast-library-pane [data-track-library-gui]');
    const form = library?.querySelector('[data-track-library-search]');
    const rows = library?.querySelector('[data-track-library-rows]');
    if (!library || !form || !rows) return;
    const pane = library.closest('#broadcast-library-pane');
    const query = form.elements.namedItem('query');
    const field = form.elements.namedItem('field');
    const sourceControl = library.querySelector('[data-broadcast-catalog-source]');
    const add = library.querySelector('[data-track-library-add]');
    const replace = library.querySelector('[data-catalog-replace]');
    const edit = library.querySelector('[data-edit-catalog-track]');
    const saveProgramme = library.querySelector('[data-catalog-save-programme]');
    const dialog = document.getElementById('broadcast-catalog-editor');
    const editor = dialog?.querySelector('[data-broadcast-catalog-form]');
    const mime = 'application/x-radios-programme-items';
    const kindLabels = { music: 'Parça', jingle: 'Jingle', announcement: 'Anons', advert: 'Reklam', programme: 'Program' };
    const aliases = { advertisement: 'advert', program: 'programme', news: 'announcement', voice_track: 'announcement' };
    const markerKeys = ['cueIn', 'intro', 'out', 'fadeStart', 'fadeEnd', 'autoNext', 'bCueIn', 'fadeIn', 'fadeOut', 'overlap', 'ducking'];
    const programmeKey = 'radios.programme-library.v1';
    const overridesKey = 'radios.broadcast-catalog-overrides.v1';
    let catalog = [], visible = [], databaseRecords = [];
    let databaseAvailable = false;
    let databaseLabel = '';
    let selectedId = null;
    let kind = 'music';
    let source = 'local';
    let requestMode = 'insert';
    let requestedBlockId = null;
    let sequence = 0;
    let editing = null;
    let previousFocus = null;
    let refreshing = false;
    const overrides = new Map();
    const text = (value, limit = 160) => typeof value === 'string' ? value.trim().slice(0, limit) : '';
    const copy = (value) => JSON.parse(JSON.stringify(value));
    const normalize = (value) => String(value ?? '').toLocaleLowerCase('tr-TR').normalize('NFD').replace(/\p{M}/gu, '').replace(/ı/g, 'i').trim();
    const validDuration = (value) => typeof value === 'string' && /^[0-9]{1,3}:[0-5][0-9]$/.test(value);
    const seconds = (value) => validDuration(value) ? Number(value.split(':')[0]) * 60 + Number(value.split(':')[1]) : null;
    const durationText = (value) => { const rounded = Math.round(value); return Number.isFinite(value) && value >= 0 && rounded < 60000 ? `${Math.floor(rounded / 60)}:${String(rounded % 60).padStart(2, '0')}` : ''; };
    const duration = (value) => validDuration(value?.duration) ? value.duration : typeof (value?.durationSeconds ?? value?.duration_seconds) === 'number' ? durationText(value.durationSeconds ?? value.duration_seconds) : '';
    const read = (key) => { try { return JSON.parse(localStorage.getItem(key) || 'null'); } catch { return null; } };
    const write = (key, value) => { try { localStorage.setItem(key, JSON.stringify(value)); return true; } catch { return false; } };
    const element = (tag, className, value) => { const node = document.createElement(tag); if (className) node.className = className; if (value !== undefined) node.textContent = value; return node; };
    const announce = (message) => { const target = library.querySelector('[data-track-library-status]'); if (target) target.textContent = message; };
    const selected = () => visible.find((record) => record.id === selectedId);
    const programmeSnapshot = () => { try { return window.RadioSProgramme?.getSnapshot?.() || null; } catch { return null; } };
    const queueContext = () => {
        const snapshot = programmeSnapshot();
        const blockId = snapshot ? snapshot.selectedBlockId || null : requestedBlockId;
        let locked = snapshot?.selectedItem?.readiness === 'locked';
        const locate = (items, inheritedLock = false) => { if (!Array.isArray(items)) return; for (const item of items) { const itemLocked = inheritedLock || item?.readiness === 'locked'; if (item?.id === blockId || item?.id === snapshot?.selectedLeafId) locked ||= itemLocked; if (Array.isArray(item?.contents)) locate(item.contents, itemLocked); } };
        locate(snapshot?.programme?.items);
        return { blockId, leafId: snapshot?.selectedLeafId || null, canReplace: Boolean(snapshot?.selectedLeafId && !locked) };
    };
    const normalizeMarkers = (value) => {
        const result = {};
        for (const name of markerKeys) {
            const candidate = Object.hasOwn(value?.markers || {}, name) ? value.markers[name] : value?.[name];
            if (candidate === null) result[name] = null;
            else if (typeof candidate === 'number' && Number.isFinite(candidate) && candidate >= (name === 'ducking' ? -60 : 0) && (name !== 'ducking' || candidate <= 0)) result[name] = candidate;
        }
        if (!Object.hasOwn(result, 'out') && Object.hasOwn(value?.markers || {}, 'cueOut')) { const candidate = value.markers.cueOut; if (candidate === null || typeof candidate === 'number' && Number.isFinite(candidate) && candidate >= 0) result.out = candidate; }
        if (typeof value?.markers?.automatic === 'boolean') result.automatic = value.markers.automatic;
        // Imported leaves receive new IDs; the next neighbour is resolved in the destination sequence.
        return result;
    };
    const cleanLeaf = (value, fallbackKind = 'music') => {
        if (!value || typeof value !== 'object' || !text(value.title)) return null;
        const candidate = text(value.type || value.kind || fallbackKind, 30);
        const type = aliases[candidate] || candidate;
        if (!['music', 'jingle', 'announcement', 'advert'].includes(type)) return null;
        return { type, title: text(value.title), artist: text(value.artist, 120), album: text(value.album, 120), duration: duration(value), version: text(value.version, 40) || 'v1', sourceId: text(value.sourceId || value.id || value._id, 200), notes: text(value.notes, 300), markers: normalizeMarkers(value), primaryState: 'unknown', backupState: 'unknown', readiness: 'pending' };
    };
    // Programme copies contain independent leaves, never references to the live queue.
    const flatItems = (items, depth = 0, parents = new Set()) => {
        if (!Array.isArray(items) || depth > 12 || parents.has(items)) return [];
        parents.add(items);
        const result = items.flatMap((value) => {
            if (Array.isArray(value)) return flatItems(value, depth + 1, parents);
            if (!value || typeof value !== 'object' || parents.has(value)) return [];
            const nested = Array.isArray(value.items) ? value.items : Array.isArray(value.contents) ? value.contents : Array.isArray(value.children) ? value.children : null;
            if (nested) { parents.add(value); const leaves = flatItems(nested, depth + 1, parents); parents.delete(value); return leaves; }
            const item = cleanLeaf(value);
            return item ? [item] : [];
        });
        parents.delete(items);
        return result;
    };
    const programmeDuration = (items) => items.length && items.every((item) => validDuration(item.duration)) ? durationText(items.reduce((total, item) => {
        const raw = seconds(item.duration);
        const bounded = (value, min) => Math.round(Math.max(min, Math.min(raw, value)) * 1000) / 1000;
        const cueIn = typeof item.markers?.cueIn === 'number' ? bounded(item.markers.cueIn, 0) : 0;
        const out = typeof item.markers?.out === 'number' ? bounded(item.markers.out, cueIn) : raw;
        return total + Math.max(0, out - cueIn);
    }, 0)) : '';
    const makeRecord = (value, recordKind, recordSource, origin, fallbackId) => {
        if (!value || typeof value !== 'object') return null;
        const title = text(value.title || value.name);
        if (!title || !Object.hasOwn(kindLabels, recordKind)) return null;
        const sourceId = text(value.sourceId || value.id || value._id, 200) || fallbackId;
        const items = recordKind === 'programme' ? flatItems(value.playbackItems || value.items || value.contents || value.entries) : undefined;
        const record = { id: `${recordSource}:${recordKind}:${sourceId}`, sourceId, kind: recordKind, source: recordSource, sourceLabel: origin, title, artist: text(value.artist, 120), album: text(value.album, 120), duration: recordKind === 'programme' ? programmeDuration(items) : duration(value), version: text(value.version, 40) || 'v1', notes: text(value.notes, 300), markers: normalizeMarkers(value) };
        if (items) record.items = items;
        return record;
    };
    const savedData = read(programmeKey);
    const storedProgrammes = Array.isArray(savedData) ? savedData : Array.isArray(savedData?.programmes) ? savedData.programmes : [];
    let savedProgrammes = storedProgrammes.flatMap((value, index) => {
        const record = makeRecord(value, 'programme', 'local', 'Saklanan program', `saved-${index}`);
        return record ? [record] : [];
    });
    const storedOverrides = read(overridesKey);
    if (Array.isArray(storedOverrides?.entries)) for (const entry of storedOverrides.entries) {
        if (!Array.isArray(entry) || typeof entry[0] !== 'string' || !entry[1] || typeof entry[1] !== 'object') continue;
        const value = entry[1];
        const record = makeRecord(value, value.kind, 'local', 'Katalog kopyası', entry[0]);
        if (record) overrides.set(entry[0], { title: record.title, artist: record.artist, album: record.album, duration: record.duration, version: record.version, kind: record.kind });
    }
    const applyOverride = (record) => {
        const override = overrides.get(record.id);
        return override ? { ...record, ...override, sourceLabel: `${record.sourceLabel} · Kopya`, ...(record.kind === 'programme' ? { duration: programmeDuration(record.items) } : {}) } : record;
    };
    const localRecords = () => {
        const records = [];
        let playlists = [];
        try { playlists = window.RadioSPlaylist?.snapshot?.()?.playlists || []; } catch { /* Local sources remain independent. */ }
        if (Array.isArray(playlists)) playlists.forEach((playlist, playlistIndex) => {
            if (!Array.isArray(playlist?.items)) return;
            playlist.items.forEach((item, itemIndex) => {
                const playlistId = text(playlist.id, 200) || `playlist-${playlistIndex}`;
                const trackId = text(item?.id, 200) || `track-${itemIndex}`;
                const record = makeRecord({ ...item, sourceId: `${playlistId}:${trackId}` }, 'music', 'local', `Playlist · ${text(playlist.name, 120) || 'Adsız'}`, `${playlistId}:${trackId}`);
                if (record) records.push({ ...record, playlistId, trackId, playlistName: text(playlist.name, 120) || 'Playlist' });
            });
        });
        [['station-jingle', 'İstasyon jingle’ı'], ['programme-jingle', 'Program jingle’ı']].forEach(([id, title]) => records.push(makeRecord({ id, title }, 'jingle', 'local', 'Örnek jingle', id)));
        const padMetadata = read('radios.pad-assignments.v1');
        if (padMetadata && typeof padMetadata === 'object' && !Array.isArray(padMetadata)) Object.entries(padMetadata).forEach(([id, item]) => {
            if (!id.startsWith('jingle:')) return;
            const record = makeRecord({ ...item, id: `pad-${id}` }, 'jingle', 'local', 'Jingle pedi', id);
            if (record) records.push(record);
        });
        [['station-announcement', 'İstasyon anonsu'], ['programme-announcement', 'Program anonsu']].forEach(([id, title]) => records.push(makeRecord({ id, title }, 'announcement', 'local', 'Örnek anons', id)));
        let campaigns = [];
        try { campaigns = window.RadioSAdvertWorkspace?.snapshot?.()?.campaigns || []; } catch { /* Advertising is an optional GUI source. */ }
        if (Array.isArray(campaigns)) campaigns.forEach((campaign, index) => {
            const record = makeRecord({ id: text(campaign?.id, 200) || `campaign-${index}`, title: campaign?.spot, artist: campaign?.client, album: campaign?.name, durationSeconds: campaign?.duration, version: campaign?.version, notes: '' }, 'advert', 'local', campaign?.example ? 'Örnek kampanya' : 'Kampanya', `campaign-${index}`);
            if (record) records.push(record);
        });
        const snapshot = programmeSnapshot();
        if (snapshot?.programme) {
            const current = makeRecord({ ...snapshot.programme, sourceId: 'current-programme', playbackItems: snapshot.playbackItems || snapshot.programme.items }, 'programme', 'local', 'Mevcut program', 'current-programme');
            if (current) records.push({ ...current, currentProgramme: true });
        }
        records.push(...savedProgrammes.map(copy));
        const unique = new Map();
        records.filter(Boolean).forEach((record) => { if (!unique.has(record.id)) unique.set(record.id, applyOverride(record)); });
        return [...unique.values()];
    };
    const renderProgrammeDetails = (record) => {
        const details = library.querySelector('[data-catalog-programme-detail]');
        const list = details?.querySelector('[data-catalog-programme-items]');
        if (!details || !list) return;
        details.hidden = record?.kind !== 'programme';
        const items = record?.kind === 'programme' ? record.items : [];
        const count = details.querySelector('[data-catalog-programme-count]');
        if (count) count.textContent = `(${items.length})`;
        const fragment = document.createDocumentFragment();
        items.forEach((item) => { const row = element('li', 'catalog-programme-item'); row.append(element('strong', '', item.title), element('small', '', `${kindLabels[item.type]} · ${item.duration || 'Süre bilinmiyor'}`)); fragment.append(row); });
        if (!items.length) fragment.append(element('li', 'catalog-programme-empty', 'Programda seçilebilir içerik yok.'));
        list.replaceChildren(fragment);
    };
    const syncSelection = () => {
        const record = selected();
        const context = queueContext();
        rows.querySelectorAll('[data-track-library-row]').forEach((row) => { const active = row.dataset.trackLibraryRow === selectedId; row.classList.toggle('is-selected', active); row.querySelector('[data-track-library-select]')?.setAttribute('aria-pressed', String(active)); });
        const usable = Boolean(record && (record.kind !== 'programme' || record.items.length));
        if (add) { add.disabled = !usable; add.textContent = record?.kind === 'programme' ? 'Programı ekle' : 'Bloğa ekle'; }
        if (replace) replace.disabled = !usable || record.kind === 'programme' || !context.canReplace;
        if (edit) { edit.disabled = !record || (!editor && !(record.source === 'local' && record.kind === 'music' && record.playlistId)); edit.textContent = record?.source === 'database' ? 'Kopyayı düzenle' : 'Düzenle'; }
        const mode = library.querySelector('[data-catalog-action-mode]');
        if (mode) mode.textContent = requestMode === 'replace' ? 'Seçili içeriği değiştirmek için seçim yap' : context.blockId ? requestMode === 'append' ? 'Bloğun sonuna ekle' : 'Seçili bloğa ekle' : 'Yayın sırasına ekle';
        const selection = library.querySelector('[data-track-library-selection]');
        if (selection) selection.textContent = record ? [record.title, kindLabels[record.kind], record.artist, record.album, record.duration || 'Süre bilinmiyor', record.sourceLabel].filter(Boolean).join(' · ') : 'İçerik seçilmedi.';
        for (const [name, value] of Object.entries({ title: record?.title || 'İçerik seçilmedi', artist: record?.artist || '—', album: record?.album || '—', duration: record?.duration || '—', playlist: record?.sourceLabel || '—' })) library.querySelectorAll(`[data-catalog-detail-${name}]`).forEach((node) => { node.textContent = value; });
        renderProgrammeDetails(record);
        if (saveProgramme) saveProgramme.disabled = !programmeSnapshot()?.programme;
    };
    const render = () => {
        const term = normalize(query?.value);
        const searchField = ['title', 'artist', 'album'].includes(field?.value) ? field.value : 'all';
        const filteredKind = catalog.filter((record) => record.kind === kind);
        visible = filteredKind.filter((record) => normalize(searchField === 'all' ? [record.title, record.artist, record.album, record.sourceLabel].join(' ') : record[searchField]).includes(term));
        if (!visible.some((record) => record.id === selectedId)) selectedId = visible[0]?.id || null;
        const focused = rows.contains(document.activeElement) ? document.activeElement.dataset.trackLibrarySelect : null;
        const fragment = document.createDocumentFragment();
        visible.forEach((record) => {
            const row = element('tr', 'broadcast-library-row'); row.dataset.trackLibraryRow = record.id; row.dataset.catalogKind = record.kind; row.draggable = record.kind !== 'programme' || record.items.length > 0;
            const titleCell = element('td', 'broadcast-library-title');
            const select = element('button', 'broadcast-library-select', record.title); select.type = 'button'; select.dataset.trackLibrarySelect = record.id; titleCell.append(select);
            row.append(element('td', 'broadcast-library-artist', record.artist || '—'), titleCell, element('td', 'broadcast-library-album', record.album || '—'), element('td', 'broadcast-library-kind', kindLabels[record.kind]), element('td', 'broadcast-library-time', record.duration || '—'), element('td', 'broadcast-library-source', record.sourceLabel)); fragment.append(row);
        });
        if (!visible.length) {
            const row = element('tr', 'broadcast-library-empty');
            const message = source === 'database' && !databaseAvailable ? 'Veritabanı bağlı değil' : filteredKind.length ? 'Sonuç yok' : 'Kayıt yok';
            const cell = element('td', '', message); cell.colSpan = 6; row.append(cell); fragment.append(row);
        }
        rows.replaceChildren(fragment);
        const count = library.querySelector('[data-track-library-count]'); if (count) count.textContent = `${visible.length} / ${filteredKind.length} ${kindLabels[kind].toLocaleLowerCase('tr-TR')}`;
        library.querySelectorAll('[data-broadcast-catalog-kind]').forEach((button) => button.setAttribute('aria-pressed', String(button.dataset.broadcastCatalogKind === kind)));
        const badge = library.querySelector('[data-catalog-source-badge]'); if (badge) badge.textContent = source === 'database' ? databaseAvailable ? 'Veritabanı snapshotı' : 'Veri bekleniyor' : 'Yerel GUI';
        syncSelection();
        if (focused) [...rows.querySelectorAll('[data-track-library-select]')].find((button) => button.dataset.trackLibrarySelect === focused)?.focus({ preventScroll: true });
        announce('');
    };
    const refresh = () => { if (refreshing) return; refreshing = true; try { catalog = source === 'database' ? databaseRecords.map(copy) : localRecords(); render(); } finally { refreshing = false; } };
    const payloadFor = (record, mode = 'insert') => {
        const context = queueContext();
        const items = record.kind === 'programme' ? record.items.map(copy) : [{ type: record.kind, title: record.title, artist: record.artist, album: record.album, duration: record.duration, version: record.version, notes: record.notes || record.sourceLabel, sourceId: record.sourceId, markers: copy(record.markers), readiness: 'pending', primaryState: 'unknown', backupState: 'unknown' }];
        return { kind: record.kind === 'programme' ? 'programme' : 'items', programmeName: record.kind === 'programme' ? record.title : '', playlistName: record.playlistName || record.sourceLabel, sourceId: record.sourceId, sourceLabel: record.sourceLabel, sourceProgrammeId: record.kind === 'programme' ? record.sourceId : undefined, currentProgramme: record.currentProgramme === true, mode, targetBlockId: context.blockId, items };
    };
    const send = (record, mode) => {
        if (!record || (record.kind === 'programme' && !record.items.length)) return;
        if (mode === 'replace' && !queueContext().canReplace) { announce('Değiştirilecek öğe seçili değil.'); return; }
        document.dispatchEvent(new CustomEvent('radios:add-programme-items', { detail: payloadFor(record, mode) }));
        announce(`${record.title} ${mode === 'replace' ? 'değiştirildi' : 'eklendi'}.`);
    };
    const openPlaylistEditor = (record) => {
        const tools = pane?.querySelector('.broadcast-playlist-tools'); const workspace = tools?.querySelector('[data-playlist-gui]'); const editButton = workspace?.querySelector('[data-playlist-track-edit]');
        const sourcePlaylist = window.RadioSPlaylist?.snapshot?.()?.playlists?.find((playlist) => playlist.id === record.playlistId);
        if (!workspace || !editButton || !sourcePlaylist?.items?.some((item) => item.id === record.trackId)) { refresh(); announce('Playlist kaydı bulunamadı.'); return; }
        const search = workspace.querySelector('#playlist-query'); if (search?.value) { search.value = ''; search.dispatchEvent(new Event('input', { bubbles: true })); }
        const playlistSelect = workspace.querySelector(`[data-playlist-select="${CSS.escape(record.playlistId)}"]`); if (!playlistSelect) { announce('Playlist kaydı bulunamadı.'); return; }
        tools.open = true; playlistSelect.click();
        const trackSelect = workspace.querySelector(`[data-playlist-track-select="${CSS.escape(record.trackId)}"]`); if (!trackSelect) { announce('Parça kaydı bulunamadı.'); return; }
        trackSelect.click(); if (editButton.disabled) { announce('Parça şu anda düzenlenemiyor.'); return; }
        editButton.click(); announce('');
    };
    const openEditor = (record) => {
        if (!record || !editor || !dialog) return;
        editing = copy(record); previousFocus = document.activeElement; editor.reset();
        for (const name of ['kind', 'title', 'artist', 'album', 'duration', 'version']) { const input = editor.elements.namedItem(name); if (input) { input.value = record[name] || ''; input.setCustomValidity(''); } }
        editor.elements.namedItem('duration').readOnly = record.kind === 'programme';
        dialog.querySelector('[data-catalog-editor-submit]').textContent = record.source === 'database' ? 'Aktar' : 'Kaydet';
        dialog.querySelector('[data-catalog-editor-status]').textContent = '';
        if (!dialog.open) dialog.showModal(); editor.elements.namedItem('title').focus();
    };
    if (editor) {
        editor.addEventListener('submit', (event) => {
            event.preventDefault(); if (!editing) return;
            const titleInput = editor.elements.namedItem('title'); titleInput.setCustomValidity(text(titleInput.value) ? '' : 'İçerik başlığını gir.'); if (!editor.reportValidity()) return;
            const updated = { ...copy(editing), title: text(titleInput.value), artist: text(editor.elements.namedItem('artist').value, 120), album: text(editor.elements.namedItem('album').value, 120), version: text(editor.elements.namedItem('version').value, 40) || 'v1', duration: editing.kind === 'programme' ? editing.duration : text(editor.elements.namedItem('duration').value, 6) };
            if (updated.source === 'database') send({ ...updated, sourceLabel: `${updated.sourceLabel} · Kopya` }, requestMode === 'replace' ? 'replace' : 'insert');
            else {
                overrides.set(updated.id, { title: updated.title, artist: updated.artist, album: updated.album, duration: updated.duration, version: updated.version, kind: updated.kind });
                const persisted = write(overridesKey, { version: 1, entries: [...overrides] }); refresh(); selectedId = updated.id; syncSelection();
                announce(`${updated.title} ${persisted ? 'kaydedildi' : 'güncellendi'}.`);
            }
            dialog.close();
        });
        editor.elements.namedItem('title')?.addEventListener('input', (event) => event.target.setCustomValidity(''));
        dialog.querySelectorAll('[data-catalog-editor-close]').forEach((button) => button.addEventListener('click', () => dialog.close()));
        dialog.addEventListener('close', () => { editing = null; if (previousFocus?.isConnected) previousFocus.focus({ preventScroll: true }); previousFocus = null; });
    }
    form.addEventListener('submit', (event) => { event.preventDefault(); render(); }); query?.addEventListener('input', render); field?.addEventListener('change', render);
    sourceControl?.addEventListener('change', () => { source = sourceControl.value === 'database' ? 'database' : 'local'; selectedId = null; refresh(); });
    library.querySelectorAll('[data-broadcast-catalog-kind]').forEach((button) => button.addEventListener('click', () => { if (Object.hasOwn(kindLabels, button.dataset.broadcastCatalogKind)) { kind = button.dataset.broadcastCatalogKind; selectedId = null; render(); } }));
    rows.addEventListener('click', (event) => { const row = event.target instanceof Element ? event.target.closest('[data-track-library-row]') : null; if (row && rows.contains(row)) { selectedId = row.dataset.trackLibraryRow; syncSelection(); } });
    rows.addEventListener('keydown', (event) => {
        if (!(event.target instanceof Element) || !event.target.matches('[data-track-library-select]')) return;
        const index = visible.findIndex((record) => record.id === event.target.dataset.trackLibrarySelect);
        const next = event.key === 'ArrowDown' ? Math.min(index + 1, visible.length - 1) : event.key === 'ArrowUp' ? Math.max(index - 1, 0) : event.key === 'Home' ? 0 : event.key === 'End' ? visible.length - 1 : null;
        if (next === null || next < 0) return; event.preventDefault(); selectedId = visible[next].id; syncSelection(); [...rows.querySelectorAll('[data-track-library-select]')].find((button) => button.dataset.trackLibrarySelect === selectedId)?.focus();
    });
    add?.addEventListener('click', () => send(selected(), requestMode === 'append' ? 'append' : 'insert')); replace?.addEventListener('click', () => send(selected(), 'replace'));
    edit?.addEventListener('click', () => { const record = selected(); if (record?.source === 'local' && record.kind === 'music' && record.playlistId) openPlaylistEditor(record); else openEditor(record); });
    saveProgramme?.addEventListener('click', () => {
        const snapshot = programmeSnapshot(); if (!snapshot?.programme) { announce('Program yok.'); return; }
        const id = `saved-programme-${Date.now().toString(36)}-${++sequence}`;
        const record = makeRecord({ ...snapshot.programme, sourceId: id, playbackItems: snapshot.playbackItems || snapshot.programme.items }, 'programme', 'local', 'Saklanan program', id); if (!record) return;
        savedProgrammes.push(record); const stored = write(programmeKey, { version: 1, programmes: savedProgrammes.map(copy) });
        source = 'local'; kind = 'programme'; if (sourceControl) sourceControl.value = source; query.value = ''; refresh(); selectedId = record.id; syncSelection();
        announce(`${record.title} saklandı.`);
    });
    rows.addEventListener('dragstart', (event) => {
        const row = event.target instanceof Element ? event.target.closest('[data-track-library-row]') : null; const record = visible.find((value) => value.id === row?.dataset.trackLibraryRow);
        if (!row || !record || !event.dataTransfer || (record.kind === 'programme' && !record.items.length)) { event.preventDefault(); return; }
        selectedId = record.id; syncSelection(); const payload = JSON.stringify(payloadFor(record, 'insert'));
        event.dataTransfer.effectAllowed = 'copy'; event.dataTransfer.setData(mime, payload); event.dataTransfer.setData('text/plain', `RadioS GUI:${payload}`); row.classList.add('is-dragging');
    });
    rows.addEventListener('dragend', () => rows.querySelectorAll('.is-dragging').forEach((row) => row.classList.remove('is-dragging')));
    document.addEventListener('radios:programme-library-request', (event) => {
        const detail = event.detail || {}; requestMode = ['insert', 'replace', 'append'].includes(detail.mode) ? detail.mode : 'insert'; requestedBlockId = text(detail.blockId || detail.targetBlockId, 200) || null;
        document.getElementById('broadcast-library-tab')?.click(); syncSelection(); query?.focus({ preventScroll: true });
    });
    ['radios:playlist-change', 'radios:programme-change', 'radios:advert-workspace-change', 'radios:dj-mixer-channels-change'].forEach((name) => document.addEventListener(name, refresh));
    document.addEventListener('radios:profile-change', (event) => { if (event.detail?.profile === 'broadcast') refresh(); else if (dialog?.open) dialog.close(); });
    window.addEventListener('storage', (event) => { if (event.key === 'radios.pad-assignments.v1') refresh(); });
    const setDatabaseRecords = (value) => {
        if (value === null) { databaseAvailable = false; databaseRecords = []; databaseLabel = ''; refresh(); return true; }
        if (!value || typeof value !== 'object' || !['tracks', 'programmes', 'jingles', 'announcements', 'adverts'].some((name) => Array.isArray(value[name]))) return false;
        try {
            const label = text(value.sourceLabel, 160) || 'Aktarılan veritabanı snapshotı'; const records = [];
            [['tracks', 'music'], ['jingles', 'jingle'], ['announcements', 'announcement'], ['adverts', 'advert'], ['programmes', 'programme']].forEach(([name, recordKind]) => { if (!Array.isArray(value[name])) return; value[name].forEach((item, index) => { const record = makeRecord(item, recordKind, 'database', label, `${name}-${index}`); if (record) records.push(record); }); });
            databaseRecords = [...new Map(records.map((record) => [record.id, record])).values()]; databaseLabel = label; databaseAvailable = true; refresh(); return true;
        } catch { return false; }
    };
    window.RadioSBroadcastCatalog = Object.freeze({ snapshot: () => ({ guiOnly: true, source, kind, databaseAvailable, sourceLabel: source === 'database' ? databaseLabel : 'Yerel kayıtlar', records: catalog.map(copy), selected: selected() ? copy(selected()) : null }), setDatabaseRecords });
    refresh(); if (document.readyState !== 'complete') document.addEventListener('DOMContentLoaded', refresh, { once: true });
})();
