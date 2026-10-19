(() => {
    const workspace = document.querySelector('[data-playlist-gui]');
    const list = workspace?.querySelector('[data-playlist-list]');
    const tracks = workspace?.querySelector('[data-playlist-tracks]');
    if (!workspace || !list || !tracks) return;

    const query = workspace.querySelector('#playlist-query');
    const details = workspace.querySelector('[data-playlist-details]');
    const empty = workspace.querySelector('[data-playlist-empty]');
    const playlistDialog = document.getElementById('playlist-editor');
    const trackDialog = document.getElementById('playlist-track-editor');
    const playlistForm = playlistDialog?.querySelector('form');
    const trackForm = trackDialog?.querySelector('form');
    const mime = 'application/x-radios-programme-items';
    const storageKey = 'radios-playlists-v1';
    let sequence = 0;
    let storageAvailable = true;
    const copy = (value) => JSON.parse(JSON.stringify(value));
    const newId = (prefix) => `${prefix}-${window.crypto?.randomUUID?.() || `${Date.now()}-${++sequence}`}`;
    const makeTrack = (title, artist = '', album = '', duration = '') => ({ id: newId('playlist-track'), title, artist, album, duration });
    const makePlaylist = (name, items = []) => ({ id: newId('playlist'), name, items });
    const cleanTrack = (item, preserveId = false) => {
        if (!item || typeof item !== 'object' || typeof item.title !== 'string' || !item.title.trim()) return null;
        const track = { ...copy(item), ...makeTrack(item.title.trim().slice(0, 240), String(item.artist || '').slice(0, 240), String(item.album || '').slice(0, 240), String(item.duration || '')) };
        if (preserveId && typeof item.id === 'string' && item.id) track.id = item.id;
        return track;
    };
    const playlists = [];
    try {
        const saved = JSON.parse(localStorage.getItem(storageKey) || 'null');
        if (Array.isArray(saved?.playlists)) saved.playlists.forEach((playlist) => {
            if (!playlist || typeof playlist.name !== 'string' || !playlist.name.trim() || !Array.isArray(playlist.items)) return;
            const restored = { ...copy(playlist), name: playlist.name.trim(), items: playlist.items.map((item) => cleanTrack(item, true)).filter(Boolean) };
            restored.id = typeof playlist.id === 'string' && playlist.id ? playlist.id : newId('playlist');
            if (!playlists.some((item) => item.id === restored.id)) playlists.push(restored);
        });
    } catch (_) { /* A blocked or damaged browser store starts with an empty library. */ }
    const persist = () => {
        try { localStorage.setItem(storageKey, JSON.stringify({ version: 1, playlists })); storageAvailable = true; }
        catch (_) { storageAvailable = false; }
    };
    let selectedPlaylistId = playlists[0]?.id || null;
    let selectedTrackId = playlists[0]?.items[0]?.id || null;
    let playlistEditorId = null;
    let trackEditorId = null;
    let trackEditorPlaylistId = null;
    const selectedPlaylist = () => playlists.find((playlist) => playlist.id === selectedPlaylistId);
    const selectedTrack = () => selectedPlaylist()?.items.find((track) => track.id === selectedTrackId);
    const snapshot = () => ({
        sample: false,
        guiOnly: true,
        storageAvailable,
        playlists: copy(playlists),
    });
    window.RadioSPlaylist = Object.freeze({
        snapshot,
        createPlaylist(name, items = []) {
            if (typeof name !== 'string' || !name.trim() || !Array.isArray(items)) return null;
            const playlist = makePlaylist(name.trim().slice(0, 240), items.map((item) => cleanTrack(item)).filter(Boolean));
            playlists.push(playlist); selectedPlaylistId = playlist.id; selectedTrackId = playlist.items[0]?.id || null;
            render(); return copy(playlist);
        },
        updatePlaylist(id, values) {
            const playlist = playlists.find((item) => item.id === id);
            if (!playlist || !values || typeof values !== 'object') return null;
            if (values.name !== undefined && (typeof values.name !== 'string' || !values.name.trim())) return null;
            const metadata = copy(values); delete metadata.id; delete metadata.items;
            Object.assign(playlist, metadata);
            if (values.name !== undefined) playlist.name = values.name.trim().slice(0, 240);
            render(); return copy(playlist);
        },
        deletePlaylist(id) {
            const index = playlists.findIndex((item) => item.id === id);
            if (index < 0) return false;
            playlists.splice(index, 1);
            if (selectedPlaylistId === id) { selectedPlaylistId = playlists[Math.min(index, playlists.length - 1)]?.id || null; selectedTrackId = null; }
            render(); return true;
        },
        addTracks(id, items) {
            const playlist = playlists.find((item) => item.id === id);
            if (!playlist || !Array.isArray(items)) return null;
            const added = items.map((item) => cleanTrack(item)).filter(Boolean);
            playlist.items.push(...added); render(); return copy(added);
        },
        updateTrack(playlistId, trackId, values) {
            const track = playlists.find((item) => item.id === playlistId)?.items.find((item) => item.id === trackId);
            if (!track || !values || typeof values !== 'object') return null;
            const cleaned = cleanTrack({ ...track, ...values, id: track.id }, true);
            if (!cleaned) return null;
            Object.assign(track, cleaned); render(); return copy(track);
        },
        deleteTracks(playlistId, ids) {
            const playlist = playlists.find((item) => item.id === playlistId);
            if (!playlist || !Array.isArray(ids)) return false;
            const removed = new Set(ids);
            const remaining = playlist.items.filter((item) => !removed.has(item.id));
            if (remaining.length === playlist.items.length) return false;
            playlist.items = remaining; render(); return true;
        },
        reorderTracks(playlistId, ids) {
            const playlist = playlists.find((item) => item.id === playlistId);
            if (!playlist || !Array.isArray(ids) || ids.length !== playlist.items.length || new Set(ids).size !== ids.length) return false;
            const tracksById = new Map(playlist.items.map((item) => [item.id, item]));
            if (ids.some((id) => !tracksById.has(id))) return false;
            playlist.items = ids.map((id) => tracksById.get(id)); render(); return true;
        },
    });
    const normalize = (value) => String(value).toLocaleLowerCase('tr-TR').normalize('NFD').replace(/\p{M}/gu, '').replace(/ı/g, 'i').trim();
    const trackMatches = (track, term) => normalize(`${track.title} ${track.artist} ${track.album}`).includes(term);
    const setText = (selector, value) => { const node = workspace.querySelector(selector); if (node) node.textContent = value; };
    const announce = (message) => setText('[data-playlist-status]', message);
    const element = (tag, className, text) => {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (text !== undefined) node.textContent = text;
        return node;
    };
    const button = (className, name, value) => {
        const node = element('button', className);
        node.type = 'button';
        node.dataset[name] = value;
        return node;
    };
    const hidden = (node) => { node.setAttribute('aria-hidden', 'true'); return node; };

    const render = () => {
        const term = normalize(query?.value || '');
        const matching = playlists.filter((playlist) => normalize(playlist.name).includes(term) || playlist.items.some((track) => trackMatches(track, term)));
        if (matching.length && !matching.some((playlist) => playlist.id === selectedPlaylistId)) selectedPlaylistId = matching[0].id;
        const active = selectedPlaylist();
        const visibleTracks = active?.items.filter((track) => !term || normalize(active.name).includes(term) || trackMatches(track, term)) || [];
        const displayedTracks = visibleTracks.slice(0, 100);
        if (!displayedTracks.some((track) => track.id === selectedTrackId)) selectedTrackId = displayedTracks[0]?.id || null;
        const focused = document.activeElement?.dataset;
        const focusedPlaylist = focused?.playlistSelect;
        const focusedTrack = focused?.playlistTrackSelect;
        const playlistFragment = document.createDocumentFragment();
        for (const playlist of matching) {
            const card = element('article', `ld-playlist${playlist.id === selectedPlaylistId ? ' is-selected' : ''}`);
            card.dataset.playlistDrag = playlist.id;
            card.draggable = playlist.items.length > 0;
            const select = button('ld-playlist-select', 'playlistSelect', playlist.id);
            select.setAttribute('aria-pressed', String(playlist.id === selectedPlaylistId));
            const info = element('span');
            info.append(element('strong', '', playlist.name), element('small', '', `${playlist.items.length} parça`));
            select.append(hidden(element('span', 'ld-playlist-icon', '≡')), info, hidden(element('span', 'ld-drag-mark', '⋮⋮')));
            card.append(select);
            playlistFragment.append(card);
        }
        list.replaceChildren(playlistFragment);
        empty.hidden = matching.length > 0;
        empty.querySelector('strong').textContent = playlists.length ? 'Sonuç bulunamadı' : 'Henüz playlist yok';
        empty.querySelector('[data-playlist-clear-search]').hidden = !term;
        details.hidden = !matching.length || !active;
        setText('[data-playlist-name]', active?.name || 'Playlist seçilmedi');
        setText('[data-playlist-count]', active ? `${active.items.length} parça${term && visibleTracks.length !== active.items.length ? ` · ${visibleTracks.length} eşleşme` : ''}${visibleTracks.length > displayedTracks.length ? ' · İlk 100 parça; tümü Playlist sekmesinde' : ''}` : '');

        const trackFragment = document.createDocumentFragment();
        for (const track of displayedTracks) {
            const card = element('article', `ld-track${track.id === selectedTrackId ? ' is-selected' : ''}`);
            card.dataset.playlistTrackDrag = track.id;
            card.draggable = true;
            const select = button('ld-track-select', 'playlistTrackSelect', track.id);
            select.setAttribute('aria-pressed', String(track.id === selectedTrackId));
            const info = element('span');
            info.append(element('strong', '', track.title), element('small', '', `${track.artist || 'Sanatçı belirtilmedi'} · ${track.album || 'Albüm belirtilmedi'}`));
            select.append(hidden(element('span', 'ld-track-index', String(active.items.indexOf(track) + 1).padStart(2, '0'))), info, element('span', 'ld-track-duration', track.duration || '—'));
            card.append(select);
            trackFragment.append(card);
        }
        if (!visibleTracks.length && active) trackFragment.append(element('p', 'ld-empty-tracks', 'Playlist boş'));
        tracks.replaceChildren(trackFragment);
        workspace.querySelector('[data-playlist-add-all]').disabled = !active?.items.length;
        workspace.querySelector('[data-playlist-add-track]').disabled = !selectedTrack();
        workspace.querySelector('[data-playlist-track-edit]').disabled = !selectedTrack();
        if (focusedPlaylist) list.querySelector(`[data-playlist-select="${focusedPlaylist}"]`)?.focus({ preventScroll: true });
        if (focusedTrack) tracks.querySelector(`[data-playlist-track-select="${focusedTrack}"]`)?.focus({ preventScroll: true });
        persist();
        document.dispatchEvent(new CustomEvent('radios:playlist-change', { detail: snapshot() }));
    };

    const field = (form, name) => form?.elements.namedItem(name);
    const read = (form, name) => String(field(form, name)?.value || '').trim();
    const fill = (form, values) => {
        form?.reset();
        for (const [name, value] of Object.entries(values)) {
            const input = field(form, name);
            if (input) { input.value = value; input.setCustomValidity(''); }
        }
    };
    const openPlaylistEditor = (isNew) => {
        if (!playlistForm || !playlistDialog?.showModal) return;
        const playlist = isNew ? null : selectedPlaylist();
        if (!isNew && !playlist) return;
        playlistEditorId = playlist?.id || null;
        fill(playlistForm, { name: playlist?.name || '' });
        playlistDialog.querySelector('h2').textContent = isNew ? 'Playlist oluştur' : 'Playlisti düzenle';
        playlistDialog.querySelector('[data-playlist-remove]').hidden = isNew;
        if (!playlistDialog.open) playlistDialog.showModal();
    };
    const openTrackEditor = (isNew) => {
        const playlist = selectedPlaylist();
        const track = isNew ? null : selectedTrack();
        if (!playlist || (!isNew && !track) || !trackForm || !trackDialog?.showModal) return;
        trackEditorId = track?.id || null;
        trackEditorPlaylistId = playlist.id;
        fill(trackForm, { title: track?.title || '', artist: track?.artist || '', album: track?.album || '', duration: track?.duration || '' });
        trackDialog.querySelector('h2').textContent = isNew ? 'Parça ekle' : 'Parçayı düzenle';
        trackDialog.querySelector('[data-playlist-track-remove]').hidden = isNew;
        if (!trackDialog.open) trackDialog.showModal();
    };
    const programmePayload = (playlist, items) => ({
        items: items.map((track) => ({ ...copy(track), id: undefined, sourceId: track.sourceId || track.sourceTrackId || track.id, type: 'music', time: '', notes: `Playlist: ${playlist.name}` })),
        playlistName: playlist.name,
    });
    const addToProgrammes = (wholePlaylist) => {
        const playlist = selectedPlaylist();
        const items = wholePlaylist ? playlist?.items : [selectedTrack()].filter(Boolean);
        if (!playlist || !items?.length) return;
        document.dispatchEvent(new CustomEvent('radios:add-programme-items', { detail: programmePayload(playlist, items) }));
        announce(`${items.length} parça eklendi.`);
    };

    workspace.addEventListener('click', (event) => {
        const target = event.target instanceof Element ? event.target.closest('button') : null;
        if (!target || target.disabled) return;
        if (target.matches('[data-playlist-select]')) {
            selectedPlaylistId = target.dataset.playlistSelect;
            selectedTrackId = null;
            render();
        } else if (target.matches('[data-playlist-track-select]')) {
            selectedTrackId = target.dataset.playlistTrackSelect;
            render();
        } else if (target.matches('[data-playlist-new]')) openPlaylistEditor(true);
        else if (target.matches('[data-playlist-edit]')) openPlaylistEditor(false);
        else if (target.matches('[data-playlist-track-new]')) openTrackEditor(true);
        else if (target.matches('[data-playlist-track-edit]')) openTrackEditor(false);
        else if (target.matches('[data-playlist-add-all]')) addToProgrammes(true);
        else if (target.matches('[data-playlist-add-track]')) addToProgrammes(false);
        else if (target.matches('[data-playlist-clear-search]')) { query.value = ''; render(); query.focus(); }
    });
    query?.addEventListener('input', render);
    workspace.querySelector('[data-playlist-search]')?.addEventListener('submit', (event) => { event.preventDefault(); render(); });

    for (const dialog of [playlistDialog, trackDialog]) {
        dialog?.addEventListener('click', (event) => {
            const target = event.target instanceof Element ? event.target.closest('button[data-playlist-close]') : null;
            if (target && dialog.open) dialog.close();
        });
        dialog?.addEventListener('input', (event) => { if (event.target instanceof HTMLInputElement) event.target.setCustomValidity(''); });
    }
    playlistForm?.addEventListener('submit', (event) => {
        event.preventDefault();
        const name = read(playlistForm, 'name');
        if (!name) { field(playlistForm, 'name').setCustomValidity('Playlist adını gir.'); playlistForm.reportValidity(); return; }
        if (!playlistForm.reportValidity()) return;
        let playlist = playlists.find((item) => item.id === playlistEditorId);
        if (playlist) playlist.name = name;
        else { playlist = makePlaylist(name); playlists.push(playlist); }
        selectedPlaylistId = playlist.id;
        query.value = '';
        playlistDialog.close();
        render();
        announce(`${name} kaydedildi.`);
    });
    trackForm?.addEventListener('submit', (event) => {
        event.preventDefault();
        const title = read(trackForm, 'title');
        if (!title) { field(trackForm, 'title').setCustomValidity('Parça adını gir.'); trackForm.reportValidity(); return; }
        if (!trackForm.reportValidity()) return;
        const playlist = playlists.find((item) => item.id === trackEditorPlaylistId);
        if (!playlist) return;
        let track = playlist.items.find((item) => item.id === trackEditorId);
        const values = { title, artist: read(trackForm, 'artist'), album: read(trackForm, 'album'), duration: read(trackForm, 'duration') };
        if (track) Object.assign(track, values);
        else { track = makeTrack(values.title, values.artist, values.album, values.duration); playlist.items.push(track); }
        selectedPlaylistId = playlist.id;
        selectedTrackId = track.id;
        query.value = '';
        trackDialog.close();
        render();
        announce(`${title} kaydedildi.`);
    });
    playlistDialog?.querySelector('[data-playlist-remove]')?.addEventListener('click', () => {
        const index = playlists.findIndex((playlist) => playlist.id === playlistEditorId);
        if (index < 0) return;
        const [removed] = playlists.splice(index, 1);
        selectedPlaylistId = playlists[Math.min(index, playlists.length - 1)]?.id || null;
        selectedTrackId = null;
        playlistDialog.close();
        render();
        announce(`${removed.name} silindi.`);
    });
    trackDialog?.querySelector('[data-playlist-track-remove]')?.addEventListener('click', () => {
        const playlist = playlists.find((item) => item.id === trackEditorPlaylistId);
        const index = playlist?.items.findIndex((item) => item.id === trackEditorId) ?? -1;
        if (index < 0) return;
        const [removed] = playlist.items.splice(index, 1);
        selectedTrackId = playlist.items[Math.min(index, playlist.items.length - 1)]?.id || null;
        trackDialog.close();
        render();
        announce(`${removed.title} silindi.`);
    });

    workspace.addEventListener('dragstart', (event) => {
        if (!(event.target instanceof Element) || !event.dataTransfer) return;
        const source = event.target.closest('[data-playlist-drag], [data-playlist-track-drag]');
        if (!source) return;
        let playlist;
        let items;
        if (source.dataset.playlistDrag) {
            playlist = playlists.find((item) => item.id === source.dataset.playlistDrag);
            items = playlist?.items;
        } else {
            playlist = playlists.find((item) => item.items.some((track) => track.id === source.dataset.playlistTrackDrag));
            items = playlist?.items.filter((track) => track.id === source.dataset.playlistTrackDrag);
        }
        if (!playlist || !items?.length) { event.preventDefault(); return; }
        const payload = JSON.stringify(programmePayload(playlist, items));
        event.dataTransfer.effectAllowed = 'copy';
        event.dataTransfer.setData(mime, payload);
        event.dataTransfer.setData('text/plain', `RadioS GUI:${payload}`);
        source.classList.add('is-dragging');
    });
    workspace.addEventListener('dragend', () => {
        workspace.querySelectorAll('.is-dragging').forEach((source) => source.classList.remove('is-dragging'));
    });
    render();
})();
