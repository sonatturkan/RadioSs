(() => {
    'use strict';

    const typeLabels = { music: 'Müzik', announcement: 'Anons', advert: 'Reklam', jingle: 'Jingle', programme: 'Program' };
    const readinessLabels = {
        pending: 'Kontrol bekliyor',
        ready: 'Hazır',
        missing: 'Dosya eksik',
        approval: 'Onay bekliyor',
        invalid: 'Geçersiz reklam',
        locked: 'Kilitli',
    };
    const text = (value, fallback = '—') => typeof value === 'string' && value.trim() ? value.trim() : fallback;
    // Eski kayıtlarda kalan örnek açıklama notları gösterilmez.
    const note = (item) => typeof item?.notes === 'string' && !item.notes.startsWith('Örnek GUI') ? item.notes.trim() : '';
    const setText = (attribute, key, value) => {
        document.querySelectorAll(`[${attribute}="${key}"]`).forEach((node) => { node.textContent = value; });
    };
    const durationSeconds = (value) => {
        if (typeof value !== 'string' || !/^[0-9]{1,3}:[0-5][0-9]$/.test(value)) return null;
        const [minutes, seconds] = value.split(':').map(Number);
        return minutes * 60 + seconds;
    };
    const durationText = (seconds) => `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;

    const render = (snapshot) => {
        const item = snapshot?.selectedItem;
        const fields = {
            artist: text(item?.artist, 'Belirtilmedi'),
            album: text(item?.album, 'Belirtilmedi'),
            duration: text(item?.duration),
            type: Object.hasOwn(typeLabels, item?.type) ? typeLabels[item.type] : '—',
            readiness: Object.hasOwn(readinessLabels, item?.readiness) ? readinessLabels[item.readiness] : '—',
            time: text(item?.time),
            notes: text(note(item)),
        };
        Object.entries(fields).forEach(([key, value]) => setText('data-broadcast-item-detail', key, value));
        document.querySelectorAll('[data-broadcast-item-detail="notes"]').forEach((node) => {
            node.hidden = !note(item);
            node.title = node.hidden ? '' : note(item);
        });

        const items = Array.isArray(snapshot?.programme?.items) ? (snapshot.playbackItems || snapshot.programme.items) : null;
        if (!items) {
            ['count', 'duration', 'unknown'].forEach((key) => setText('data-broadcast-flow-detail', key, '—'));
            ['announcement', 'advert'].forEach((key) => setText('data-broadcast-flow-next', key, 'Akışta yok'));
            return;
        }

        let totalSeconds = 0;
        let unknownCount = 0;
        items.forEach((entry) => {
            const seconds = durationSeconds(entry?.duration);
            if (seconds === null) unknownCount += 1;
            else totalSeconds += seconds;
        });
        setText('data-broadcast-flow-detail', 'count', String(items.length));
        const knownTotal = Number.isFinite(snapshot?.timingAnalysis?.knownSeconds) ? snapshot.timingAnalysis.knownSeconds : totalSeconds;
        const overlap = Number.isFinite(snapshot?.timingAnalysis?.overlapSeconds) ? snapshot.timingAnalysis.overlapSeconds : 0;
        setText('data-broadcast-flow-detail', 'duration', `${durationText(Math.max(0, knownTotal - overlap))}${unknownCount ? ' + ?' : ''}`);
        setText('data-broadcast-flow-detail', 'unknown', String(unknownCount));

        const firstLeafId = Array.isArray(item?.contents) ? item.contents[0]?.id : item?.id;
        const selectedIndex = firstLeafId ? items.findIndex((entry) => entry?.id === firstLeafId) : -1;
        const followingItems = selectedIndex >= 0 ? items.slice(selectedIndex + (Array.isArray(item?.contents) ? 0 : 1)) : items;
        ['announcement', 'advert'].forEach((type) => {
            const next = followingItems.find((entry) => entry?.type === type);
            setText('data-broadcast-flow-next', type, text(next?.title, 'Akışta yok'));
        });
    };

    document.addEventListener('radios:programme-change', (event) => render(event.detail));
    render(window.RadioSProgramme?.getSnapshot?.());
})();
