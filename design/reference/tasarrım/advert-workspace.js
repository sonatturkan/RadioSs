(() => {
    'use strict';
    const root = document.querySelector('[data-advert-workspace]');
    const form = root?.querySelector('[data-advert-form]');
    if (!root || !form) return;
    const storageKey = 'radios.advert-workspace.v1';
    const q = (selector) => root.querySelector(selector);
    const copy = (value) => JSON.parse(JSON.stringify(value));
    const text = (value, limit = 120) => typeof value === 'string' ? value.trim().slice(0, limit) : '';
    const integer = (value, min, max, fallback) => Number.isInteger(Number(value)) && Number(value) >= min && Number(value) <= max ? Number(value) : fallback;
    const positionLabels = { any: 'Herhangi', first: 'İlk', last: 'Son' };
    const fallbackLabels = { skip: 'Eksik spotu atla', approvedAlternative: 'Onaylı alternatifi kullan', operator: 'Operatör kararı bekle' };
    const choice = (value, labels, fallback) => Object.hasOwn(labels, value) ? value : fallback;
    const validDate = (value) => typeof value === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(value) && Number.isFinite(new Date(`${value}T00:00:00Z`).getTime()) && new Date(`${value}T00:00:00Z`).toISOString().slice(0, 10) === value;
    const validTime = (value) => /^(?:[01]\d|2[0-3]):[0-5]\d$/.test(value);
    const today = () => new Date().toISOString().slice(0, 10);
    const base = { name: '', client: '', owner: '', reference: '', start: '', end: '', target: '', spot: '', version: 'v1', duration: 30, count: 0, price: null, currency: 'TRY', contact: '', slot: '', capacity: 0, intro: 0, outro: 0, announcement: 0, competitorGroup: '', position: 'any', skipEmptyJingle: true, fallback: 'skip', approved: false, revision: 1, history: [], example: false };
    let sequence = 2;
    let dialogOrigin = null;
    const normalizeHistory = (entries) => Array.isArray(entries) ? entries.flatMap((entry) => {
        if (!entry || !text(entry.spot) || !text(entry.version, 40) || !validDate(entry.start) || !validDate(entry.end) || !Number.isFinite(new Date(entry.savedAt).getTime())) return [];
        return [{ spot: text(entry.spot), version: text(entry.version, 40), duration: integer(entry.duration, 1, 3600, 30), start: entry.start, end: entry.end, savedAt: new Date(entry.savedAt).toISOString(), revision: integer(entry.revision, 1, 1000000000, 1) }];
    }) : [];
    const normalizeCampaign = (entry) => {
        if (!entry || typeof entry !== 'object' || !text(entry.name) || !text(entry.spot) || !text(entry.version, 40) || !validDate(entry.start) || !validDate(entry.end) || entry.end < entry.start) return null;
        const price = entry.price === null || entry.price === '' || entry.price === undefined ? null : Number(entry.price);
        let id = /^advert-campaign-[1-9]\d*$/.test(entry.id) ? entry.id : `advert-campaign-${++sequence}`;
        const number = Number(id.split('-').at(-1));
        if (!Number.isSafeInteger(number) || number > 1000000) id = `advert-campaign-${++sequence}`;
        else sequence = Math.max(sequence, number);
        return { ...base, id, name: text(entry.name), client: text(entry.client), owner: text(entry.owner), reference: text(entry.reference), start: entry.start, end: entry.end, target: text(entry.target), spot: text(entry.spot), version: text(entry.version, 40), duration: integer(entry.duration, 1, 3600, 30), count: integer(entry.count, 0, 100000, 0), price: Number.isFinite(price) && price !== null && price >= 0 && price <= 1000000000 ? Math.round(price * 100) / 100 : null, currency: ['TRY', 'EUR', 'USD'].includes(entry.currency) ? entry.currency : 'TRY', contact: text(entry.contact, 1000), slot: validTime(entry.slot) ? entry.slot : '', capacity: integer(entry.capacity, 0, 86400, 0), intro: integer(entry.intro, 0, 3600, 0), outro: integer(entry.outro, 0, 3600, 0), announcement: integer(entry.announcement, 0, 3600, 0), competitorGroup: text(entry.competitorGroup, 100), position: choice(entry.position, positionLabels, 'any'), skipEmptyJingle: entry.skipEmptyJingle !== false, fallback: choice(entry.fallback, fallbackLabels, 'skip'), approved: entry.approved === true, revision: integer(entry.revision, 1, 1000000000, 1), history: normalizeHistory(entry.history), example: entry.example === true };
    };
    const examples = () => [
        { ...copy(base), id: 'advert-campaign-1', name: 'Örnek açılış kampanyası', spot: 'Tanıtım spotu', version: 'v1', start: '2026-10-03', end: '2026-10-31', duration: 30, count: 24, reference: 'GUI-001', example: true },
        { ...copy(base), id: 'advert-campaign-2', name: 'Örnek hafta sonu kampanyası', spot: 'Etkinlik spotu', version: 'v2', start: '2026-10-03', end: '2026-10-18', duration: 20, count: 12, reference: 'GUI-002', example: true },
    ];
    let state = { campaigns: examples(), selectedId: 'advert-campaign-1', search: '', notificationFilter: 'all', readKeys: [] };
    try {
        const stored = JSON.parse(localStorage.getItem(storageKey) || 'null');
        if (stored?.version === 1 && Array.isArray(stored.campaigns)) {
            const ids = new Set();
            state.campaigns = stored.campaigns.flatMap((entry) => { const campaign = normalizeCampaign(entry); if (!campaign || ids.has(campaign.id)) return []; ids.add(campaign.id); return [campaign]; });
            state.selectedId = ids.has(stored.selectedId) ? stored.selectedId : state.campaigns[0]?.id || '';
            state.search = text(stored.search, 120);
            state.notificationFilter = stored.notificationFilter === 'unread' ? 'unread' : 'all';
            state.readKeys = Array.isArray(stored.readKeys) ? [...new Set(stored.readKeys.filter((key) => typeof key === 'string' && key.length < 300))].slice(-500) : [];
            sequence = Math.max(sequence, integer(stored.lastSequence, 2, 1000000, 2));
        }
    } catch { /* Corrupt data restores the clearly labelled example drafts. */ }
    const create = (tag, className = '', content) => {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (content !== undefined) node.textContent = content;
        return node;
    };
    const field = (name) => form.elements.namedItem(name);
    const announce = (message) => { q('[data-advert-status]').textContent = message; };
    const actionButton = (label, action, id = '', description = '') => {
        const button = create('button', '', label);
        button.type = 'button';
        button.dataset.advertAction = action;
        if (id) button.dataset.advertId = id;
        if (description) button.setAttribute('aria-label', description);
        if (action === 'edit' || action === 'notification-edit') { button.setAttribute('aria-haspopup', 'dialog'); button.setAttribute('aria-controls', 'advert-campaign-dialog'); }
        return button;
    };
    const slotDuration = (campaign) => campaign.duration + campaign.intro + campaign.outro + campaign.announcement;
    const hash = (value) => {
        let code = 2166136261;
        for (const letter of JSON.stringify(value)) { code ^= letter.charCodeAt(0); code = Math.imul(code, 16777619); }
        return (code >>> 0).toString(36);
    };
    const notifications = () => {
        const day = today();
        const read = new Set(state.readKeys);
        const list = [];
        state.campaigns.forEach((campaign) => {
            const add = (type, level, title, impact, signature) => list.push({ key: `${campaign.id}:${campaign.revision}:${type}:${hash(signature)}`, type, level, title, campaignId: campaign.id, campaignName: campaign.name, target: campaign.target || 'Atanmadı', owner: campaign.owner || 'Atanmadı', impact });
            if (!campaign.approved) add('approval', 'warning', 'Onay bekliyor', '', [campaign.version, campaign.spot, campaign.start, campaign.end, campaign.reference, campaign.target]);
            if (campaign.end < day) add('expired', 'warning', 'Kampanya süresi dolmuş', `Bitiş ${dateText(campaign.end)}`, campaign.end);
            else if (campaign.start > day) add('upcoming', 'info', 'Kampanya tarihi başlamadı', `Başlangıç ${dateText(campaign.start)}`, campaign.start);
            if (campaign.capacity > 0 && slotDuration(campaign) > campaign.capacity) add('capacity', 'warning', 'Kuşak kapasitesi aşılıyor', `Bir spot + intro / outro / sabit anons ${slotDuration(campaign)} sn; kuşak kapasitesi ${campaign.capacity} sn.`, [campaign.duration, campaign.intro, campaign.outro, campaign.announcement, campaign.capacity]);
        });
        return list.map((notification) => ({ ...notification, read: read.has(notification.key) }));
    };
    const snapshot = () => ({ ...copy(state), notifications: copy(notifications()) });
    const persist = () => {
        try { localStorage.setItem(storageKey, JSON.stringify({ version: 1, ...state, lastSequence: sequence })); return true; }
        catch { return false; }
    };
    const commit = (message) => {
        const stored = persist();
        render();
        document.dispatchEvent(new CustomEvent('radios:advert-workspace-change', { detail: snapshot() }));
        if (message) announce(`${message}${stored ? '' : ' Kalıcı kayıt yapılamadı.'}`);
    };
    const selected = () => state.campaigns.find((campaign) => campaign.id === state.selectedId);
    const dateText = (value) => validDate(value) ? new Date(`${value}T12:00:00Z`).toLocaleDateString('tr-TR') : '—';
    const filtered = () => {
        const query = state.search.toLocaleLowerCase('tr-TR');
        return state.campaigns.filter((campaign) => !query || [campaign.name, campaign.client, campaign.spot, campaign.owner, campaign.reference, campaign.target, campaign.version].join(' ').toLocaleLowerCase('tr-TR').includes(query));
    };
    const renderCampaigns = () => {
        const host = q('[data-advert-campaign-list]');
        const visible = filtered();
        if (!visible.length) {
            const row = create('tr');
            const cell = create('td', 'advert-empty', state.campaigns.length ? 'Sonuç yok' : 'Kampanya yok');
            cell.colSpan = 4;
            row.append(cell);
            host.replaceChildren(row);
            return;
        }
        host.replaceChildren(...visible.map((campaign) => {
            const row = create('tr', campaign.id === state.selectedId ? 'advert-row-selected' : '');
            row.dataset.advertCampaign = campaign.id;
            const name = create('td');
            const choose = actionButton(campaign.name, 'select', campaign.id, `${campaign.name} kampanyasını seç`);
            choose.setAttribute('aria-pressed', String(campaign.id === state.selectedId));
            name.append(choose, create('p', 'advert-meta', `${campaign.spot} · ${campaign.version}`));
            if (campaign.example) name.append(create('small', 'advert-meta', 'Örnek'));
            const dates = create('td', '', `${dateText(campaign.start)} → ${dateText(campaign.end)}`);
            const plan = create('td', '', `${campaign.count} adet · ${campaign.duration} sn`);
            const approval = create('td');
            approval.append(create('span', 'advert-badge', campaign.approved ? 'Onaylı' : 'Onay bekliyor'));
            row.append(name, dates, plan, approval);
            return row;
        }));
    };
    const renderDetails = () => {
        const campaign = selected();
        q('[data-advert-detail-name]').textContent = campaign?.name || 'Kampanya seçilmedi';
        q('[data-advert-detail-spot]').textContent = campaign ? `${campaign.spot} · ${campaign.version} · ${campaign.duration} sn` : '—';
        q('[data-advert-detail-approval]').textContent = campaign ? campaign.approved ? 'Onaylı' : 'Onay bekliyor' : '—';
        root.querySelectorAll('[data-advert-detail-actions] button').forEach((button) => { button.disabled = !campaign; button.dataset.advertId = campaign?.id || ''; });
        const approvalButton = q('[data-advert-detail-actions] [data-advert-action="approve"]');
        approvalButton.textContent = campaign?.approved ? 'Onayı geri al' : 'Onayla';
        approvalButton.setAttribute('aria-pressed', String(campaign?.approved === true));
        const facts = campaign ? {
            people: `${campaign.client || 'Reklamveren belirtilmedi'} · ${campaign.owner || 'Sorumlu atanmadı'}`,
            contract: campaign.reference || 'Belirtilmedi', dates: `${dateText(campaign.start)} → ${dateText(campaign.end)}`,
            target: campaign.target || 'Atanmadı', plan: `${campaign.count} spot · ${campaign.count * campaign.duration} sn toplam spot süresi`,
            slot: campaign.slot ? `${campaign.slot} · planlanan ilk kuşak` : 'Belirlenmedi',
            price: campaign.price === null ? 'Belirtilmedi' : `${campaign.price.toLocaleString('tr-TR', { minimumFractionDigits: 2, maximumFractionDigits: 2 })} ${campaign.currency}`,
            contact: campaign.contact || 'Belirtilmedi',
            capacity: campaign.capacity ? `${slotDuration(campaign)} / ${campaign.capacity} sn · ${slotDuration(campaign) > campaign.capacity ? 'Kapasite aşılıyor' : 'Bir spot plana sığıyor'}` : `Belirlenmedi · bir spotun kuşak süresi ${slotDuration(campaign)} sn`,
            overhead: `${campaign.intro} / ${campaign.outro} / ${campaign.announcement} sn`,
            competition: `${campaign.competitorGroup || 'Grup belirtilmedi'} · ${positionLabels[campaign.position]}`,
            distribution: `${campaign.skipEmptyJingle ? 'Boş kuşakta jingle atlanır' : 'Boş kuşakta jingle korunur'} · ${fallbackLabels[campaign.fallback]}`,
        } : {};
        root.querySelectorAll('[data-advert-fact]').forEach((output) => { output.textContent = facts[output.dataset.advertFact] || '—'; });
        q('[data-advert-planned-count]').textContent = campaign ? String(campaign.count) : '—';
        const history = q('[data-advert-version-history]');
        if (!campaign?.history.length) history.replaceChildren(create('p', 'advert-meta', campaign ? 'Önceki sürüm yok' : '—'));
        else {
            const list = create('ol');
            [...campaign.history].reverse().forEach((entry) => {
                const row = create('li');
                row.append(create('strong', '', `${entry.spot} · ${entry.version} · ${entry.duration} sn`), create('p', 'advert-meta', `${dateText(entry.start)} → ${dateText(entry.end)} · ${new Date(entry.savedAt).toLocaleString('tr-TR', { dateStyle: 'short', timeStyle: 'short' })}`));
                list.append(row);
            });
            history.replaceChildren(list);
        }
    };
    const renderNotifications = () => {
        const all = notifications();
        const unread = all.filter((notification) => !notification.read).length;
        q('[data-advert-unread-count]').value = `${unread} okunmamış`;
        root.querySelectorAll('[data-advert-notification-filter]').forEach((button) => button.setAttribute('aria-pressed', String(button.dataset.advertNotificationFilter === state.notificationFilter)));
        const visible = all.filter((notification) => state.notificationFilter !== 'unread' || !notification.read);
        const host = q('[data-advert-notification-list]');
        if (!visible.length) { host.replaceChildren(create('p', 'advert-empty', 'Bildirim yok')); return; }
        host.replaceChildren(...visible.map((notification) => {
            const row = create('article', `advert-notification advert-note-${notification.level}${notification.read ? ' is-read' : ''}`);
            row.dataset.advertNotificationKey = notification.key;
            const main = create('div', 'advert-notification-main');
            main.append(create('h4', '', `${notification.level === 'warning' ? 'Uyarı' : notification.level === 'error' ? 'Hata' : 'Bilgi'} · ${notification.title}`), create('p', 'advert-meta', notification.campaignName), ...(notification.impact ? [create('p', '', notification.impact)] : []), create('p', 'advert-meta', `Hedef: ${notification.target} · Sorumlu: ${notification.owner}`));
            const actions = create('div', 'advert-actions');
            if (notification.campaignId) actions.append(actionButton('İlgili kampanyayı aç', 'notification-open', notification.campaignId), actionButton('Düzenle', 'notification-edit', notification.campaignId));
            const read = actionButton(notification.read ? 'Okundu' : 'Okundu işaretle', 'read');
            read.dataset.advertKey = notification.key;
            read.disabled = notification.read;
            actions.append(read);
            row.append(main, actions);
            return row;
        }));
    };
    const render = () => {
        const focused = document.activeElement?.closest('[data-advert-action]');
        const focus = focused && root.contains(focused) ? { action: focused.dataset.advertAction, id: focused.dataset.advertId, key: focused.dataset.advertKey } : null;
        renderCampaigns();
        renderDetails();
        renderNotifications();
        if (focus && !focused.isConnected) {
            const candidates = [...root.querySelectorAll('[data-advert-action]')];
            const replacement = candidates.find((button) => button.dataset.advertAction === focus.action && button.dataset.advertId === focus.id && button.dataset.advertKey === focus.key && !button.disabled);
            if (replacement) replacement.focus({ preventScroll: true });
            else if (focus.action === 'read') q(`[data-advert-notification-filter="${state.notificationFilter}"]`).focus({ preventScroll: true });
        }
    };
    const selectCampaign = (id) => {
        if (!state.campaigns.some((campaign) => campaign.id === id)) return;
        state.selectedId = id;
        state.search = '';
        q('[data-advert-search]').value = '';
    };
    const setFields = (campaign) => {
        Object.entries(campaign).forEach(([name, value]) => {
            const input = field(name);
            if (!input) return;
            if (input.type === 'checkbox') input.checked = value === true;
            else input.value = value === null ? '' : String(value ?? '');
        });
    };
    const capacityPreview = () => {
        const values = ['duration', 'intro', 'outro', 'announcement'].map((name) => Number(field(name).value) || 0);
        const total = values.reduce((sum, value) => sum + value, 0);
        const capacity = Number(field('capacity').value) || 0;
        q('[data-advert-capacity-preview]').textContent = `Bir spot ${values[0]} + intro ${values[1]} + outro ${values[2]} + sabit anons ${values[3]} = ${total} sn. ${capacity ? `Kuşak ${capacity} sn: ${total > capacity ? 'kapasite aşılıyor' : 'plana sığıyor'}.` : 'Kuşak kapasitesi belirlenmedi.'}`;
        q('[data-advert-capacity-preview]').classList.toggle('has-warning', Boolean(capacity && total > capacity));
    };
    const openCampaign = (campaign = null, origin = null) => {
        form.reset();
        [...form.elements].forEach((input) => { if ('setCustomValidity' in input) input.setCustomValidity(''); });
        const defaultDay = today();
        setFields(campaign || { ...copy(base), id: '', start: defaultDay, end: defaultDay });
        q('#advert-campaign-dialog-title').textContent = campaign ? 'Kampanyayı düzenle' : 'Kampanya ekle';
        q('[data-advert-form-status]').textContent = '';
        q('.advert-advanced-form').open = false;
        capacityPreview();
        dialogOrigin = origin || document.activeElement;
        q('#advert-campaign-dialog').showModal();
        field('name').focus();
    };
    const previousVersion = (campaign) => ({ spot: campaign.spot, version: campaign.version, duration: campaign.duration, start: campaign.start, end: campaign.end, revision: campaign.revision, savedAt: new Date().toISOString() });
    const editableKeys = Object.keys(base).filter((key) => !['approved', 'revision', 'history', 'example'].includes(key));
    const readDraft = () => {
        const candidate = { ...copy(base) };
        editableKeys.forEach((name) => {
            const input = field(name);
            if (!input) return;
            if (input.type === 'checkbox') candidate[name] = input.checked;
            else if (input.type === 'number') candidate[name] = name === 'price' && input.value === '' ? null : Number(input.value);
            else candidate[name] = text(input.value, name === 'contact' ? 1000 : name === 'competitorGroup' ? 100 : name === 'version' ? 40 : 120);
        });
        return candidate;
    };
    form.addEventListener('submit', (event) => {
        event.preventDefault();
        [...form.elements].forEach((input) => {
            if ('setCustomValidity' in input) input.setCustomValidity('');
            if (input.type === 'text' || input.tagName === 'TEXTAREA') input.value = input.value.trim();
        });
        if (field('end').value < field('start').value) field('end').setCustomValidity('Bitiş tarihi başlangıçtan önce olamaz.');
        if (!form.reportValidity()) return;
        const existing = state.campaigns.find((campaign) => campaign.id === field('id').value);
        const candidate = readDraft();
        const changed = existing && editableKeys.some((key) => candidate[key] !== existing[key]);
        const versionChanged = existing && ['spot', 'version', 'duration', 'start', 'end'].some((key) => candidate[key] !== existing[key]);
        candidate.id = existing?.id || `advert-campaign-${++sequence}`;
        candidate.history = existing ? copy(existing.history) : [];
        if (versionChanged) candidate.history.push(previousVersion(existing));
        candidate.revision = existing ? existing.revision + (changed ? 1 : 0) : 1;
        candidate.approved = existing && !changed ? existing.approved : false;
        candidate.example = existing?.example === true;
        if (existing) state.campaigns[state.campaigns.indexOf(existing)] = candidate;
        else state.campaigns.push(candidate);
        selectCampaign(candidate.id);
        q('#advert-campaign-dialog').close();
        commit(`${candidate.name} ${existing ? 'güncellendi' : 'eklendi'}.${changed || !existing ? ' Onay gerekiyor.' : ''}`);
        q('[data-advert-detail-actions] [data-advert-action="edit"]').focus({ preventScroll: true });
    });
    form.addEventListener('input', () => {
        [...form.elements].forEach((input) => { if ('setCustomValidity' in input) input.setCustomValidity(''); });
        capacityPreview();
    });
    form.addEventListener('change', capacityPreview);
    form.addEventListener('invalid', (event) => {
        const disclosure = event.target.closest('details');
        if (disclosure) disclosure.open = true;
    }, true);
    root.querySelectorAll('[data-advert-close]').forEach((button) => button.addEventListener('click', () => q('#advert-campaign-dialog').close()));
    q('#advert-campaign-dialog').addEventListener('close', () => { if (dialogOrigin?.isConnected && !dialogOrigin.closest('[hidden]')) dialogOrigin.focus({ preventScroll: true }); dialogOrigin = null; });
    q('[data-advert-new]').addEventListener('click', (event) => openCampaign(null, event.currentTarget));
    const download = (campaign) => {
        const blob = new Blob([JSON.stringify({ format: 'radios-local-advert-plan', kind: 'Yerel kampanya planı', campaign: copy(campaign), actualEvidence: null, mainReadiness: 'Veri bekleniyor', backupReadiness: 'Veri bekleniyor' }, null, 2)], { type: 'application/json;charset=utf-8' });
        const address = URL.createObjectURL(blob);
        const link = create('a');
        link.href = address;
        link.download = `radios-${campaign.id}-plan.json`;
        document.body.append(link);
        link.click();
        link.remove();
        setTimeout(() => URL.revokeObjectURL(address), 1000);
        announce(`${campaign.name} planı indirildi.`);
    };
    root.addEventListener('click', (event) => {
        const filter = event.target.closest('[data-advert-notification-filter]');
        if (filter) { state.notificationFilter = filter.dataset.advertNotificationFilter; commit(); return; }
        const button = event.target.closest('[data-advert-action]');
        if (!button || !root.contains(button)) return;
        const action = button.dataset.advertAction;
        if (action === 'read') {
            const key = button.dataset.advertKey;
            if (!notifications().some((notification) => notification.key === key)) return;
            state.readKeys = [...new Set([...state.readKeys, key])].slice(-500);
            commit('Bildirim okundu işaretlendi. İlgili plan koşulu değişmedi.');
            return;
        }
        const campaign = state.campaigns.find((entry) => entry.id === (button.dataset.advertId || state.selectedId));
        if (!campaign) return;
        if (action === 'select' || action === 'notification-open') {
            selectCampaign(campaign.id);
            commit();
            if (action === 'notification-open') q('[data-advert-details]')?.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
        }
        if (action === 'edit' || action === 'notification-edit') {
            selectCampaign(campaign.id);
            render();
            const replacement = root.querySelector(`[data-advert-action="${action}"][data-advert-id="${CSS.escape(campaign.id)}"]`);
            openCampaign(campaign, replacement || q('[data-advert-detail-actions] [data-advert-action="edit"]'));
        }
        if (action === 'approve') {
            campaign.approved = !campaign.approved;
            campaign.revision += 1;
            commit(`${campaign.name}: ${campaign.approved ? 'onaylandı' : 'onay geri alındı'}.`);
        }
        if (action === 'remove') {
            state.campaigns = state.campaigns.filter((entry) => entry.id !== campaign.id);
            if (state.selectedId === campaign.id) state.selectedId = state.campaigns[0]?.id || '';
            commit(`${campaign.name} silindi.`);
            q('[data-advert-new]').focus({ preventScroll: true });
        }
        if (action === 'download') download(campaign);
    });
    q('[data-advert-search]').addEventListener('input', (event) => {
        state.search = text(event.target.value, 120);
        const visible = filtered();
        if (!visible.some((campaign) => campaign.id === state.selectedId)) state.selectedId = visible[0]?.id || '';
        persist();
        render();
    });
    document.addEventListener('radios:profile-change', (event) => { if (event.detail?.profile === 'advert') render(); else if (q('#advert-campaign-dialog').open) q('#advert-campaign-dialog').close(); });
    q('[data-advert-search]').value = state.search;
    const initialVisible = filtered();
    if (!initialVisible.some((campaign) => campaign.id === state.selectedId)) state.selectedId = initialVisible[0]?.id || '';
    window.RadioSAdvertWorkspace = Object.freeze({ snapshot });
    render();
})();
