(() => {
  'use strict';
  const root = document.querySelector('[data-playlist-workspace]');
  if (!root) return;
  const q = (selector, scope = root) => scope.querySelector(selector);
  const qa = (selector, scope = root) => [...scope.querySelectorAll(selector)];
  const api = () => window.RadioSPlaylist;
  const normalize = (text) => String(text ?? '').toLocaleLowerCase('tr-TR').normalize('NFD').replace(/\p{M}/gu, '').replace(/ı/g, 'i').trim();
  const element = (tag, className = '', text) => { const node = document.createElement(tag); if (className) node.className = className; if (text !== undefined) node.textContent = text; return node; };
  const button = (text, data, value) => { const node = element('button', '', text); node.type = 'button'; node.dataset[data] = value; return node; };
  const text = (selector, value) => { const node = q(selector); if (node) node.textContent = value; };
  const announce = (message, error = false) => { text('[data-pw-status]', message); q('[data-pw-status]').classList.toggle('is-error', error); };
  const seconds = (duration) => { const match = /^(\d+):([0-5]\d)$/.exec(String(duration || '')); return match ? Number(match[1]) * 60 + Number(match[2]) : null; };
  const time = (total) => { if (!Number.isFinite(total) || total < 0) return ''; const rounded = Math.round(total); return `${Math.floor(rounded / 60)}:${String(rounded % 60).padStart(2, '0')}`; };
  const decodedLocation = (location) => { try { return decodeURIComponent(String(location || '')); } catch (_) { return String(location || ''); } };
  const identity = (track) => track.location ? `location:${decodedLocation(track.location)}` : track.sourceTrackId ? `rekordbox:${track.sourceTrackId}` : null;
  const children = (node, name) => [...(node?.children || [])].filter((child) => child.localName === name);
  const attributes = (node) => Object.fromEntries([...node.attributes].map((attribute) => [attribute.name, attribute.value]));
  const parseRekordboxXml = (source) => {
    if (typeof source !== 'string' || !source.trim()) throw new Error('XML dosyası boş.');
    if (/<!DOCTYPE|<!ENTITY/i.test(source)) throw new Error('DOCTYPE veya dış varlık içeren XML desteklenmiyor.');
    const documentXml = new DOMParser().parseFromString(source, 'application/xml');
    if (documentXml.querySelector('parsererror')) throw new Error('XML okunamadı. Geçerli bir rekordbox XML dosyası seç.');
    const documentRoot = documentXml.documentElement;
    if (documentRoot.localName !== 'DJ_PLAYLISTS') throw new Error('Bu dosya rekordbox koleksiyon XML biçiminde değil.');
    const version = documentRoot.getAttribute('Version');
    if (version && version !== '1.0.0') throw new Error(`Desteklenmeyen XML sürümü: ${version}. Desteklenen sürüm 1.0.0.`);
    const collectionNode = children(documentRoot, 'COLLECTION')[0];
    if (!collectionNode) throw new Error('XML içinde COLLECTION parça koleksiyonu bulunamadı.');
    const collectionTracks = children(collectionNode, 'TRACK');
    if (collectionTracks.length > 100000) throw new Error('Bir dosyada en fazla 100.000 parça içeri aktarılabilir.');
    const collection = [], byId = new Map(), byLocation = new Map(), playlists = [];
    let missingReferences = 0, duplicateIds = 0, missingLocations = 0;
    for (const node of collectionTracks) {
      const raw = attributes(node), location = raw.Location || '';
      const durationSeconds = raw.TotalTime !== undefined && raw.TotalTime !== '' ? Number(raw.TotalTime) : NaN;
      const baseName = decodedLocation(location).split(/[\\/]/).pop() || '';
      const cues = children(node, 'POSITION_MARK').map((mark) => attributes(mark));
      const grid = children(node, 'TEMPO').map((mark) => attributes(mark));
      const track = { title: raw.Name || baseName || 'Adsız parça', artist: raw.Artist || '', album: raw.Album || '', duration: time(durationSeconds), sourceTrackId: raw.TrackID || '', location, sourceLocation: location, genre: raw.Genre || '', bpm: Number.isFinite(Number(raw.AverageBpm)) && raw.AverageBpm !== undefined && raw.AverageBpm !== '' ? Number(raw.AverageBpm) : '', year: raw.Year || '', rating: Number.isFinite(Number(raw.Rating)) && raw.Rating !== undefined ? Number(raw.Rating) / 51 : '', key: raw.Tonality || '', comment: raw.Comments || '', cues, grid, rekordbox: { attributes: raw, cues, grid } };
      collection.push(track);
      if (!location) missingLocations++;
      if (track.sourceTrackId) { if (byId.has(track.sourceTrackId)) duplicateIds++; else byId.set(track.sourceTrackId, track); }
      if (location && !byLocation.has(decodedLocation(location))) byLocation.set(decodedLocation(location), track);
    }
    const walk = (node, folders = [], depth = 0, isRoot = false) => {
      if (depth > 64) throw new Error('XML playlist klasörleri çok derin. En fazla 64 düzey destekleniyor.');
      const name = node.getAttribute('Name') || (node.getAttribute('Type') === '1' ? 'Adsız playlist' : 'Adsız klasör');
      if (node.getAttribute('Type') === '1') {
        const keyType = node.getAttribute('KeyType') || '0';
        if (!['0', '1'].includes(keyType)) throw new Error(`Desteklenmeyen playlist anahtar türü: ${keyType}.`);
        const items = [];
        for (const reference of children(node, 'TRACK')) {
          const key = reference.getAttribute('Key') || '';
          const track = keyType === '1' ? byLocation.get(decodedLocation(key)) : byId.get(key);
          if (track) items.push(track); else missingReferences++;
        }
        playlists.push({ name: [...folders, name].join(' / '), folder: folders.join(' / '), items });
      } else {
        const nextFolders = isRoot && name.toUpperCase() === 'ROOT' ? folders : [...folders, name];
        for (const child of children(node, 'NODE')) walk(child, nextFolders, depth + 1);
      }
    };
    const playlistRoot = children(documentRoot, 'PLAYLISTS')[0];
    for (const node of children(playlistRoot, 'NODE')) walk(node, [], 0, true);
    return { collection, playlists, missingReferences, duplicateIds, missingLocations };
  };
  window.RadioSPlaylistWorkspace = Object.freeze({ parseRekordboxXml });

  let selectedPlaylistId = null, activeTrackId = null, selectedIds = new Set(), trackEditorId = null, trackEditorPlaylistId = null, nameEditorId = null, deletingId = null, imported = null, importFileName = '', readSequence = 0, pageIndex = 0;
  const pageSize = 100;
  const snapshot = () => api()?.snapshot?.() || { playlists: [], storageAvailable: false };
  const current = () => snapshot().playlists.find((playlist) => playlist.id === selectedPlaylistId);
  const activeTrack = () => current()?.items.find((track) => track.id === activeTrackId);
  const trackMatches = (track, term) => normalize(`${track.title} ${track.artist} ${track.album} ${track.genre || ''} ${track.comment || ''}`).includes(term);
  const filteredTracks = (playlist = current()) => {
    const term = normalize(q('[data-pw-track-search]').value);
    const list = playlist?.items.filter((track) => !term || trackMatches(track, term)) || [];
    const sort = q('[data-pw-sort]').value;
    if (sort !== 'order') list.sort((a, b) => sort === 'duration' ? (seconds(a.duration) ?? Infinity) - (seconds(b.duration) ?? Infinity) : sort === 'bpm' ? (Number(a.bpm) || Infinity) - (Number(b.bpm) || Infinity) : String(a[sort] || '').localeCompare(String(b[sort] || ''), 'tr', { numeric: true }));
    return list;
  };
  const visibleTracks = (playlist = current()) => filteredTracks(playlist).slice(pageIndex * pageSize, (pageIndex + 1) * pageSize);
  const render = () => {
    const store = snapshot(), playlists = store.playlists;
    if (!playlists.some((playlist) => playlist.id === selectedPlaylistId)) { selectedPlaylistId = playlists[0]?.id || null; activeTrackId = null; selectedIds.clear(); }
    const playlist = playlists.find((item) => item.id === selectedPlaylistId);
    const availableIds = new Set(playlist?.items.map((track) => track.id) || []);
    selectedIds = new Set([...selectedIds].filter((id) => availableIds.has(id)));
    if (!availableIds.has(activeTrackId)) activeTrackId = [...selectedIds][0] || null;
    const term = normalize(q('[data-pw-list-search]').value);
    const matching = playlists.filter((item) => !term || normalize(item.name).includes(term) || item.items.some((track) => trackMatches(track, term)));
    const focused = document.activeElement?.dataset || {};
    const focusedList = focused.pwList, focusedTrack = focused.pwTrack, focusedCheck = focused.pwTrackCheck;
    const listFragment = document.createDocumentFragment();
    for (const item of matching) {
      const node = button('', 'pwList', item.id);
      node.setAttribute('aria-pressed', String(item.id === selectedPlaylistId));
      node.append(element('strong', '', item.name), element('small', '', `${item.items.length} parça`));
      listFragment.append(node);
    }
    if (!matching.length) listFragment.append(element('p', 'pw-empty', playlists.length ? 'Aramana uygun playlist yok.' : 'Henüz playlist yok. Yeni liste oluştur veya XML içeri aktar.'));
    q('[data-pw-lists]').replaceChildren(listFragment);
    text('[data-pw-list-count]', playlists.length);
    text('[data-pw-track-count]', playlists.reduce((count, item) => count + item.items.length, 0));
    text('[data-pw-storage]', store.storageAvailable === false ? 'Tarayıcıya kaydedilemedi · bu oturumda kullanılabilir' : 'Bu tarayıcıda saklanır');
    text('[data-pw-name]', playlist?.name || 'Playlist seçilmedi');
    const knownTimes = playlist?.items.map((track) => seconds(track.duration)).filter((value) => value !== null) || [];
    text('[data-pw-details]', playlist ? `${playlist.items.length} parça · ${time(knownTimes.reduce((sum, value) => sum + value, 0))} bilinen süre${knownTimes.length < playlist.items.length ? ` · ${playlist.items.length - knownTimes.length} süre bilinmiyor` : ''}` : 'Playlist oluştur veya rekordbox XML dosyanı içeri aktar.');
    const filtered = filteredTracks(playlist);
    pageIndex = Math.min(pageIndex, Math.max(0, Math.ceil(filtered.length / pageSize) - 1));
    const visible = filtered.slice(pageIndex * pageSize, (pageIndex + 1) * pageSize), trackFragment = document.createDocumentFragment();
    const trackPositions = new Map(playlist?.items.map((track, index) => [track.id, index + 1]) || []);
    for (const track of visible) {
      const row = element('tr', selectedIds.has(track.id) ? 'is-selected' : '');
      const checkCell = element('td'), checkbox = element('input');
      checkbox.type = 'checkbox'; checkbox.dataset.pwTrackCheck = track.id; checkbox.checked = selectedIds.has(track.id); checkbox.setAttribute('aria-label', `${track.title} seç`); checkCell.append(checkbox);
      const titleCell = element('td'), select = button(track.title, 'pwTrack', track.id);
      select.setAttribute('aria-pressed', String(activeTrackId === track.id));
      titleCell.append(select, element('small', '', track.artist || 'Sanatçı belirtilmedi'));
      row.append(checkCell, element('td', '', trackPositions.get(track.id)), titleCell, element('td', '', track.album || '—'), element('td', '', track.genre || '—'), element('td', '', track.bpm || '—'), element('td', '', track.duration || '—'));
      trackFragment.append(row);
    }
    if (!visible.length) { const cell = element('td', 'pw-empty', !playlist ? 'Playlist oluştur veya XML içeri aktar.' : playlist.items.length ? 'Aramana uygun parça bulunamadı.' : 'Playlist boş. Parça ekle veya XML içeri aktar.'); cell.colSpan = 7; const row = element('tr'); row.append(cell); trackFragment.append(row); }
    q('[data-pw-tracks]').replaceChildren(trackFragment);
    const selectAll = q('[data-pw-select-all]'); selectAll.checked = visible.length > 0 && visible.every((track) => selectedIds.has(track.id)); selectAll.indeterminate = visible.some((track) => selectedIds.has(track.id)) && !selectAll.checked; selectAll.disabled = !visible.length;
    for (const selector of ['[data-pw-rename]', '[data-pw-delete]', '[data-pw-track-new]']) q(selector).disabled = !playlist;
    q('[data-pw-track-edit]').disabled = selectedIds.size !== 1;
    q('[data-pw-track-remove]').disabled = !selectedIds.size;
    q('[data-pw-add-selected]').disabled = !selectedIds.size;
    q('[data-pw-add-all]').disabled = !playlist?.items.length;
    qa('[data-pw-export]').forEach((node) => { node.disabled = !playlist?.items.length; });
    const activeIndex = playlist?.items.findIndex((track) => track.id === [...selectedIds][0]) ?? -1;
    q('[data-pw-up]').disabled = selectedIds.size !== 1 || activeIndex < 1;
    q('[data-pw-down]').disabled = selectedIds.size !== 1 || activeIndex < 0 || activeIndex >= playlist.items.length - 1;
    text('[data-pw-selection]', `${selectedIds.size} parça seçili · ${filtered.length} eşleşme`);
    text('[data-pw-page-info]', filtered.length ? `${pageIndex * pageSize + 1}–${pageIndex * pageSize + visible.length} / ${filtered.length} parça` : '0 parça');
    const previousPage = q('[data-pw-page-prev]'), nextPage = q('[data-pw-page-next]');
    if (previousPage) previousPage.disabled = pageIndex === 0;
    if (nextPage) nextPage.disabled = (pageIndex + 1) * pageSize >= filtered.length;
    const track = activeTrack(); q('[data-pw-source]').hidden = !track;
    text('[data-pw-location]', track?.location || 'Dosya konumu belirtilmedi');
    text('[data-pw-source-id]', track?.sourceTrackId || '—');
    text('[data-pw-cues]', track ? `${track.key || 'Ton belirtilmedi'} · ${track.cues?.length || 0} cue işareti · ${track.grid?.length || 0} tempo noktası` : '—');
    text('[data-pw-comment]', track?.comment || '—');
    if (focusedList) q(`[data-pw-list="${CSS.escape(focusedList)}"]`)?.focus({ preventScroll: true });
    if (focusedTrack) q(`[data-pw-track="${CSS.escape(focusedTrack)}"]`)?.focus({ preventScroll: true });
    if (focusedCheck) q(`[data-pw-track-check="${CSS.escape(focusedCheck)}"]`)?.focus({ preventScroll: true });
  };
  const mutatable = () => { if (!api()?.createPlaylist) { announce('Playlist deposu hazır değil. Sayfayı yenile.', true); return false; } return true; };
  const clearSelection = () => { selectedIds.clear(); activeTrackId = null; pageIndex = 0; q('[data-pw-track-search]').value = ''; q('[data-pw-sort]').value = 'order'; };
  const formValue = (form, name) => String(form.elements.namedItem(name)?.value || '').trim();
  const setForm = (form, data) => { form.reset(); for (const [name, value] of Object.entries(data)) { const field = form.elements.namedItem(name); if (field) { field.value = value ?? ''; field.setCustomValidity(''); } } };
  const nameDialog = q('#pw-name-dialog'), trackDialog = q('#pw-track-dialog'), importDialog = q('#pw-import-dialog'), deleteDialog = q('#pw-delete-dialog');
  const nameForm = q('[data-pw-name-form]'), trackForm = q('[data-pw-track-form]');
  const openTrack = (isNew) => {
    const playlist = current(), track = isNew ? null : playlist?.items.find((item) => item.id === [...selectedIds][0]);
    if (!playlist || (!isNew && !track)) return;
    trackEditorId = track?.id || null; trackEditorPlaylistId = playlist.id;
    setForm(trackForm, { title: track?.title || '', artist: track?.artist || '', album: track?.album || '', duration: track?.duration || '', bpm: track?.bpm || '', genre: track?.genre || '', tonality: track?.key || '', location: track?.location || '', comment: track?.comment || '' });
    text('#pw-track-title', isNew ? 'Parça ekle' : 'Parçayı düzenle'); trackDialog.showModal();
  };
  const toProgramme = (all) => {
    const playlist = current(), items = playlist?.items.filter((item) => all || selectedIds.has(item.id)) || [];
    if (!items.length) return;
    document.dispatchEvent(new CustomEvent('radios:add-programme-items', { detail: { playlistName: playlist.name, items: items.map((track) => ({ ...track, type: 'music', time: '', notes: `Playlist: ${playlist.name}${track.comment ? ` · ${track.comment}` : ''}` })) } }));
    announce(`${items.length} parça yayın sırasına eklendi.`);
  };
  const moveTrack = (offset) => {
    const playlist = current(); if (!playlist || selectedIds.size !== 1 || !mutatable()) return;
    const ids = playlist.items.map((track) => track.id), index = ids.indexOf([...selectedIds][0]), target = index + offset;
    if (index < 0 || target < 0 || target >= ids.length) return;
    [ids[index], ids[target]] = [ids[target], ids[index]];
    if (api().reorderTracks(playlist.id, ids)) { q('[data-pw-sort]').value = 'order'; render(); announce('Playlist sırası güncellendi.'); }
  };
  const download = (content, filename, type) => {
    const uri = URL.createObjectURL(new Blob([content], { type })), link = element('a');
    link.href = uri; link.download = filename; document.body.append(link); link.click(); link.remove(); window.setTimeout(() => URL.revokeObjectURL(uri), 1000);
  };
  const exportPlaylist = (kind) => {
    const playlist = current(); if (!playlist?.items.length) return;
    const filename = playlist.name.replace(/[<>:"/\\|?*\u0000-\u001f]/g, '_').slice(0, 100) || 'playlist';
    if (kind === 'csv') {
      const cell = (value) => { let data = String(value ?? ''); if (/^[=+\-@\t\r]/.test(data)) data = `'${data}`; return `"${data.replace(/"/g, '""')}"`; };
      const rows = [['Sıra', 'Parça', 'Sanatçı', 'Albüm', 'Süre', 'BPM', 'Tür', 'Ton', 'Dosya', 'rekordbox kimliği', 'Yorum'], ...playlist.items.map((track, index) => [index + 1, track.title, track.artist, track.album, track.duration, track.bpm, track.genre, track.key, track.location, track.sourceTrackId, track.comment])];
      download(`\ufeff${rows.map((row) => row.map(cell).join(',')).join('\r\n')}\r\n`, `${filename}.csv`, 'text/csv;charset=utf-8'); announce('Playlist CSV olarak indirildi.');
    } else {
      const items = playlist.items.filter((track) => track.location && !/[\r\n]/.test(track.location));
      if (!items.length) { announce('M3U8 için dosya konumu olan bir parça gerekli.', true); return; }
      const clean = (value) => String(value || '').replace(/[\r\n]/g, ' ');
      const lines = ['#EXTM3U', ...items.flatMap((track) => [`#EXTINF:${seconds(track.duration) ?? -1},${clean(track.artist)}${track.artist ? ' - ' : ''}${clean(track.title)}`, track.location])];
      download(`${lines.join('\n')}\n`, `${filename}.m3u8`, 'audio/x-mpegurl;charset=utf-8'); announce(`${items.length} parça M3U8 olarak indirildi.${items.length < playlist.items.length ? ` Dosya konumu olmayan ${playlist.items.length - items.length} parça atlandı.` : ''}`);
    }
  };
  q('[data-pw-create]').addEventListener('submit', (event) => {
    event.preventDefault(); const form = event.currentTarget, name = formValue(form, 'name');
    if (!name || !form.reportValidity() || !mutatable()) return;
    const playlist = api().createPlaylist(name); if (!playlist) return;
    selectedPlaylistId = playlist.id; clearSelection(); q('[data-pw-list-search]').value = ''; form.reset(); render(); announce(`${playlist.name} oluşturuldu.`);
  });
  nameForm.addEventListener('submit', (event) => {
    event.preventDefault(); const name = formValue(nameForm, 'name'); if (!name || !nameForm.reportValidity() || !mutatable()) return;
    const playlist = api().updatePlaylist(nameEditorId, { name }); if (!playlist) { announce('Düzenlenen playlist artık bulunamıyor.', true); nameDialog.close(); return; }
    nameDialog.close(); render(); announce('Playlist adı güncellendi.');
  });
  trackForm.addEventListener('submit', (event) => {
    event.preventDefault(); const title = formValue(trackForm, 'title'); if (!title || !trackForm.reportValidity() || !mutatable()) return;
    const values = { title, artist: formValue(trackForm, 'artist'), album: formValue(trackForm, 'album'), duration: formValue(trackForm, 'duration'), bpm: formValue(trackForm, 'bpm') === '' ? '' : Number(formValue(trackForm, 'bpm')), genre: formValue(trackForm, 'genre'), key: formValue(trackForm, 'tonality'), location: formValue(trackForm, 'location'), comment: formValue(trackForm, 'comment') };
    const track = trackEditorId ? api().updateTrack(trackEditorPlaylistId, trackEditorId, values) : api().addTracks(trackEditorPlaylistId, [values])?.[0];
    if (!track) { announce('Parça kaydedilemedi. Playlistin mevcut olduğunu kontrol et.', true); return; }
    selectedPlaylistId = trackEditorPlaylistId; selectedIds = new Set([track.id]); activeTrackId = track.id; q('[data-pw-track-search]').value = ''; trackDialog.close(); render(); announce('Parça kaydedildi.');
  });
  deleteDialog.addEventListener('close', () => { if (deleteDialog.returnValue !== 'delete' || !deletingId || !mutatable()) return; if (api().deletePlaylist(deletingId)) { clearSelection(); render(); announce('Playlist silindi.'); } deletingId = null; });
  root.addEventListener('click', (event) => {
    const target = event.target.closest('button'); if (!target || target.disabled) return;
    if (target.dataset.pwList) { selectedPlaylistId = target.dataset.pwList; clearSelection(); render(); }
    else if (target.dataset.pwTrack) { activeTrackId = target.dataset.pwTrack; selectedIds = new Set([activeTrackId]); render(); }
    else if (target.hasAttribute('data-pw-rename')) { const playlist = current(); if (playlist) { nameEditorId = playlist.id; setForm(nameForm, { name: playlist.name }); nameDialog.showModal(); } }
    else if (target.hasAttribute('data-pw-delete')) { const playlist = current(); if (playlist) { deletingId = playlist.id; text('[data-pw-delete-message]', `${playlist.name} (${playlist.items.length} parça kaydı) silinecek.`); deleteDialog.returnValue = ''; deleteDialog.showModal(); } }
    else if (target.hasAttribute('data-pw-track-new')) openTrack(true);
    else if (target.hasAttribute('data-pw-track-edit')) openTrack(false);
    else if (target.hasAttribute('data-pw-track-remove')) { const playlist = current(); if (playlist && selectedIds.size && mutatable()) { const count = selectedIds.size; if (api().deleteTracks(playlist.id, [...selectedIds])) { clearSelection(); render(); announce(`${count} parça playlistten çıkarıldı.`); } } }
    else if (target.hasAttribute('data-pw-up')) moveTrack(-1);
    else if (target.hasAttribute('data-pw-down')) moveTrack(1);
    else if (target.hasAttribute('data-pw-page-prev')) { pageIndex = Math.max(0, pageIndex - 1); render(); }
    else if (target.hasAttribute('data-pw-page-next')) { pageIndex++; render(); }
    else if (target.hasAttribute('data-pw-add-selected')) toProgramme(false);
    else if (target.hasAttribute('data-pw-add-all')) toProgramme(true);
    else if (target.dataset.pwExport) exportPlaylist(target.dataset.pwExport);
    else if (target.hasAttribute('data-pw-import-open')) { importDialog.showModal(); }
    else if (target.hasAttribute('data-pw-close')) target.closest('dialog')?.close();
    else if (target.dataset.pwImportSelect) { qa('[data-pw-import-choice]').forEach((node) => { node.checked = target.dataset.pwImportSelect === 'all'; }); updateImportSubmit(); }
  });
  root.addEventListener('change', (event) => {
    const target = event.target;
    if (target.matches('[data-pw-track-check]')) { if (target.checked) { selectedIds.add(target.dataset.pwTrackCheck); activeTrackId = target.dataset.pwTrackCheck; } else selectedIds.delete(target.dataset.pwTrackCheck); render(); }
    else if (target.matches('[data-pw-select-all]')) { for (const track of visibleTracks()) { if (target.checked) selectedIds.add(track.id); else selectedIds.delete(track.id); } activeTrackId = [...selectedIds][0] || null; render(); }
    else if (target.matches('[data-pw-sort]')) { pageIndex = 0; render(); }
    else if (target.matches('[data-pw-import-choice], [data-pw-import-collection]')) updateImportSubmit();
  });
  for (const selector of ['[data-pw-list-search]', '[data-pw-track-search]']) q(selector).addEventListener('input', () => { pageIndex = 0; render(); });
  const updateImportSubmit = () => { q('[data-pw-import-submit]').disabled = !imported || (!qa('[data-pw-import-choice]:checked').length && !(q('[data-pw-import-collection]').checked && imported.collection.length)); };
  q('[data-pw-import-file]').addEventListener('change', async (event) => {
    const token = ++readSequence, file = event.target.files?.[0]; imported = null; q('[data-pw-import-preview]').hidden = true; updateImportSubmit();
    q('[data-pw-import-status]').classList.remove('is-error');
    if (!file) { text('[data-pw-import-status]', 'Dosya seçilmedi.'); return; }
    if (file.size > 20 * 1024 * 1024) { text('[data-pw-import-status]', 'XML dosyası en fazla 20 MB olabilir.'); q('[data-pw-import-status]').classList.add('is-error'); return; }
    text('[data-pw-import-status]', 'XML okunuyor…');
    try {
      const source = await file.text(); if (token !== readSequence) return;
      const parsed = parseRekordboxXml(source); imported = parsed; importFileName = file.name;
      const fragment = document.createDocumentFragment();
      parsed.playlists.forEach((playlist, index) => { const label = element('label', 'pw-check'), check = element('input'); check.type = 'checkbox'; check.checked = true; check.dataset.pwImportChoice = String(index); const info = element('span'); info.append(element('strong', '', playlist.name), element('small', '', `${playlist.items.length} parça${playlist.folder ? ` · Klasör: ${playlist.folder}` : ''}`)); label.append(check, info); fragment.append(label); });
      if (!parsed.playlists.length) fragment.append(element('p', 'pw-empty', 'Dosyada playlist yok. Tüm koleksiyonu tek liste olarak içeri aktarabilirsin.'));
      q('[data-pw-import-lists]').replaceChildren(fragment); q('[data-pw-import-collection]').checked = !parsed.playlists.length && parsed.collection.length > 0;
      q('[data-pw-import-collection]').disabled = !parsed.collection.length; q('[data-pw-import-preview]').hidden = false;
      const notices = [parsed.missingReferences ? `${parsed.missingReferences} çözülemeyen parça bağlantısı atlandı` : '', parsed.duplicateIds ? `${parsed.duplicateIds} tekrarlanan kimlikte ilk kayıt kullanıldı` : '', parsed.missingLocations ? `${parsed.missingLocations} parçanın dosya konumu yok` : ''].filter(Boolean);
      text('[data-pw-import-status]', `${file.name}: ${parsed.collection.length} koleksiyon parçası · ${parsed.playlists.length} playlist.${notices.length ? ` ${notices.join('; ')}.` : ''}`); updateImportSubmit();
    } catch (error) { if (token !== readSequence) return; text('[data-pw-import-status]', error.message || 'Dosya okunamadı.'); q('[data-pw-import-status]').classList.add('is-error'); }
  });
  q('[data-pw-import-form]').addEventListener('submit', (event) => {
    event.preventDefault(); if (!imported || !mutatable()) return;
    const sources = qa('[data-pw-import-choice]:checked').map((check) => imported.playlists[Number(check.dataset.pwImportChoice)]).filter(Boolean);
    if (q('[data-pw-import-collection]').checked && imported.collection.length) sources.push({ name: `${importFileName.replace(/\.xml$/i, '')} / Tüm parçalar`, items: imported.collection });
    if (!sources.length) return;
    const merge = q('[data-pw-name-policy]').value === 'merge', skip = q('[data-pw-duplicate-policy]').value === 'skip';
    let lists = 0, tracks = 0, skipped = 0, lastPlaylistId = null;
    for (const source of sources) {
      let existing = merge ? snapshot().playlists.find((playlist) => normalize(playlist.name) === normalize(source.name)) : null;
      const seen = new Set(skip ? (existing?.items || []).map(identity).filter(Boolean) : []);
      const items = source.items.filter((track) => { const key = identity(track); if (skip && key && seen.has(key)) { skipped++; return false; } if (key) seen.add(key); return true; });
      if (existing) { const added = api().addTracks(existing.id, items); if (added) { tracks += added.length; lists++; lastPlaylistId = existing.id; } }
      else {
        const names = new Set(snapshot().playlists.map((playlist) => normalize(playlist.name)));
        const base = source.name.slice(0, 120); let name = base, suffix = 2;
        while (names.has(normalize(name))) { const ending = ` (${suffix++})`; name = `${base.slice(0, 120 - ending.length)}${ending}`; }
        existing = api().createPlaylist(name, items); if (existing) { api().updatePlaylist(existing.id, { source: 'rekordbox', sourceFolder: source.folder || '', importedFrom: importFileName }); tracks += existing.items.length; lists++; lastPlaylistId = existing.id; }
      }
    }
    if (!lists) { text('[data-pw-import-status]', 'İçe aktarma tamamlanamadı. Playlist deposunu kontrol et.'); return; }
    selectedPlaylistId = lastPlaylistId; clearSelection(); q('[data-pw-list-search]').value = ''; importDialog.close(); render(); announce(`${lists} playlist işlendi · ${tracks} parça eklendi${skipped ? ` · ${skipped} tekrar atlandı` : ''}. Dosya konumları korundu.`);
  });
  document.addEventListener('radios:playlist-change', render);
  document.addEventListener('radios:profile-change', (event) => { if (event.detail?.profile === 'playlist') render(); else for (const dialog of [nameDialog, trackDialog, importDialog, deleteDialog]) if (dialog.open) dialog.close(); });
  render();
})();
