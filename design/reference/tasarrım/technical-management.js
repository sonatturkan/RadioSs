(() => {
    'use strict';

    const root = document.querySelector('[data-technical-management]');
    if (!root) return;
    const storageKey = 'radios.technical-management.v1';
    const architectureLabels = { local: 'Yerel', cloud: 'Bulut', hybrid: 'Hibrit' };
    const serverRoleLabels = { primary: 'Ana yayın', backup: 'Yedek yayın', management: 'Yönetim' };
    const personRoleLabels = { operator: 'Yayın operatörü', presenter: 'Sunucu / DJ', technical: 'Teknik yönetici', advert: 'Reklam sorumlusu', custom: 'Özel rol' };
    const scopeLabels = { station: 'İstasyon', programme: 'Program', player: 'Oynatıcı' };
    const authLabels = { password: 'Parola', mfa: 'Parola + ikinci doğrulama', identity: 'Kurum kimliği' };
    const permissionLabels = { monitor: 'Yalnız izleme', control: 'Yayın kontrolü', limited: 'Seçili kapsam ve yetkiler' };
    const accessLabels = { monitor: 'İzleme', control: 'Yayın kontrolü', cue: 'CUE / ön dinleme', programmeEdit: 'Program düzenleme', errorFix: 'Hata düzeltme', serviceManagement: 'Servis yönetimi', bulkTemporary: 'Geçici toplu düzenleme', bulkPermanent: 'Kalıcı toplu düzenleme' };
    const webDefaults = { managementUrl: '', listenerUrl: '', siteUrl: '', apiPath: '/api/radios', auth: 'password', sessionMinutes: 30, permission: 'monitor', siteSearch: false, siteRequests: false, siteAdverts: false };
    const q = (selector) => root.querySelector(selector);
    const serverForm = q('[data-tech-server-form]');
    const personForm = q('[data-tech-person-form]');
    const webForm = q('[data-tech-web-form]');
    if (!serverForm || !personForm || !webForm) return;
    let serverSequence = 0;
    let personSequence = 0;
    let targets = { programme: [], player: [] };
    const dialogFocus = new WeakMap();
    const clone = (value) => JSON.parse(JSON.stringify(value));
    const text = (value, limit = 500) => typeof value === 'string' ? value.trim().slice(0, limit) : '';
    const choice = (value, labels, fallback) => Object.hasOwn(labels, value) ? value : fallback;
    const integer = (value, min, max, fallback) => Number.isInteger(Number(value)) && Number(value) >= min && Number(value) <= max ? Number(value) : fallback;
    const dateValue = (value) => typeof value === 'string' && /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}$/.test(value) && Number.isFinite(new Date(value).getTime()) ? value : '';
    const urlValue = (value, httpsOnly = false) => {
        const candidate = text(value);
        if (!candidate) return '';
        try {
            const url = new URL(candidate);
            if (!(httpsOnly ? url.protocol === 'https:' : ['https:', 'http:'].includes(url.protocol)) || !url.hostname || url.username || url.password) return null;
            return url.href;
        } catch { return null; }
    };
    const safePath = (value) => {
        const candidate = text(value, 200);
        return /^\/(?!\/)[^\s\\]*$/.test(candidate) ? candidate : null;
    };
    const create = (tag, className = '', content) => {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (content !== undefined) node.textContent = content;
        return node;
    };
    const field = (form, name) => form.elements.namedItem(name);
    const formText = (form, name, limit) => text(field(form, name)?.value, limit);
    const checked = (form, name) => Boolean(field(form, name)?.checked);
    const announce = (message) => { const status = q('[data-tech-status]'); if (status) status.textContent = message; };
    const setFields = (form, values) => {
        Object.entries(values).forEach(([name, value]) => {
            const input = field(form, name);
            if (!input) return;
            if (input.type === 'checkbox') input.checked = value === true;
            else input.value = String(value ?? '');
        });
    };
    const stripForm = (form) => {
        [...form.elements].forEach((input) => {
            if ('setCustomValidity' in input) input.setCustomValidity('');
            if (['text', 'url'].includes(input.type)) input.value = input.value.trim();
        });
    };
    const validateUrl = (form, name, httpsOnly) => {
        const input = field(form, name);
        const result = urlValue(input.value, httpsOnly);
        if (result === null) input.setCustomValidity(`${httpsOnly ? 'HTTPS' : 'HTTP veya HTTPS'} adresi gir. Adreste kullanıcı adı veya parola bulunmamalı.`);
        return result;
    };
    const normalizeServer = (entry) => {
        if (!entry || typeof entry !== 'object' || !text(entry.name, 100) || !text(entry.station, 100)) return null;
        const address = urlValue(entry.address);
        if (!address) return null;
        let id = /^technical-server-[1-9]\d*$/.test(entry.id) ? entry.id : `technical-server-${++serverSequence}`;
        const number = Number(id.split('-').at(-1));
        if (!Number.isSafeInteger(number) || number > 1000000) id = `technical-server-${++serverSequence}`;
        else serverSequence = Math.max(serverSequence, number);
        return { id, name: text(entry.name, 100), architecture: choice(entry.architecture, architectureLabels, 'local'), role: choice(entry.role, serverRoleLabels, 'primary'), station: text(entry.station, 100), address, database: text(entry.database, 100), offlineMinutes: integer(entry.offlineMinutes, 0, 100800, 0) };
    };
    const normalizePerson = (entry) => {
        if (!entry || typeof entry !== 'object' || !text(entry.name, 100) || !text(entry.login, 120)) return null;
        const role = choice(entry.role, personRoleLabels, 'operator');
        if (role === 'custom' && !text(entry.customRole, 100)) return null;
        const scope = choice(entry.scope, scopeLabels, 'station');
        const station = text(entry.station, 100);
        const target = text(entry.target, 200);
        if (scope === 'station' ? !station : !target) return null;
        const start = dateValue(entry.start);
        const end = dateValue(entry.end);
        const period = entry.period === 'temporary' ? 'temporary' : 'permanent';
        if (period === 'temporary' && !end || start && end && new Date(end) <= new Date(start)) return null;
        let id = /^technical-person-[1-9]\d*$/.test(entry.id) ? entry.id : `technical-person-${++personSequence}`;
        const number = Number(id.split('-').at(-1));
        if (!Number.isSafeInteger(number) || number > 1000000) id = `technical-person-${++personSequence}`;
        else personSequence = Math.max(personSequence, number);
        const permissions = Object.fromEntries(Object.keys(accessLabels).map((name) => [name, entry.permissions?.[name] === true]));
        return { id, name: text(entry.name, 100), login: text(entry.login, 120), role, customRole: role === 'custom' ? text(entry.customRole, 100) : '', scope, station, target, targetName: text(entry.targetName, 120), server: text(entry.server, 100), serverRemoved: entry.serverRemoved === true, start, end, period, permissions };
    };
    const normalizeWeb = (entry) => {
        if (!entry || typeof entry !== 'object') return { ...webDefaults };
        return {
            managementUrl: urlValue(entry.managementUrl, true) || '',
            listenerUrl: urlValue(entry.listenerUrl) || '',
            siteUrl: urlValue(entry.siteUrl, true) || '',
            apiPath: safePath(entry.apiPath) || webDefaults.apiPath,
            auth: choice(entry.auth, authLabels, 'password'),
            sessionMinutes: integer(entry.sessionMinutes, 1, 1440, 30),
            permission: choice(entry.permission, permissionLabels, 'monitor'),
            siteSearch: entry.siteSearch === true, siteRequests: entry.siteRequests === true, siteAdverts: entry.siteAdverts === true,
        };
    };
    let state = { servers: [], people: [], web: { ...webDefaults }, audit: [] };
    try {
        const stored = JSON.parse(localStorage.getItem(storageKey) || 'null');
        if (stored?.version === 1 && Array.isArray(stored.servers) && Array.isArray(stored.people)) {
            const serverIds = new Set();
            const personIds = new Set();
            state.servers = stored.servers.flatMap((entry) => { const value = normalizeServer(entry); if (!value || serverIds.has(value.id)) return []; serverIds.add(value.id); return [value]; });
            state.people = stored.people.flatMap((entry) => { const value = normalizePerson(entry); if (!value || personIds.has(value.id)) return []; personIds.add(value.id); return [value]; });
            state.people.forEach((person) => { if (person.server && !serverIds.has(person.server)) { person.server = ''; person.serverRemoved = true; } });
            state.web = normalizeWeb(stored.web);
            state.audit = Array.isArray(stored.audit) ? stored.audit.flatMap((entry) => entry && text(entry.message, 300) && Number.isFinite(new Date(entry.at).getTime()) ? [{ at: new Date(entry.at).toISOString(), message: text(entry.message, 300) }] : []).slice(0, 20) : [];
        }
    } catch { state = { servers: [], people: [], web: { ...webDefaults }, audit: [] }; }
    const snapshot = () => clone(state);
    const save = (message) => {
        state.audit.unshift({ at: new Date().toISOString(), message });
        state.audit = state.audit.slice(0, 20);
        let stored = true;
        try { localStorage.setItem(storageKey, JSON.stringify({ version: 1, ...state })); } catch { stored = false; }
        render();
        document.dispatchEvent(new CustomEvent('radios:technical-management-change', { detail: snapshot() }));
        announce(`${message}${stored ? '' : ' Kalıcı kayıt yapılamadı.'}`);
    };
    const actionButton = (label, action, id, description) => {
        const button = create('button', '', label);
        button.type = 'button';
        button.dataset.techAction = action;
        button.dataset.techId = id;
        button.setAttribute('aria-label', description);
        if (action.startsWith('edit')) { button.setAttribute('aria-haspopup', 'dialog'); button.setAttribute('aria-controls', action === 'edit-server' ? 'technical-server-dialog' : 'technical-person-dialog'); }
        return button;
    };
    const empty = (host, title, description) => {
        const node = create('div', 'technical-empty');
        node.append(create('h4', '', title));
        host.replaceChildren(node);
    };
    const renderServers = () => {
        const host = q('[data-tech-server-list]');
        if (!state.servers.length) { empty(host, 'Sunucu tanımlanmadı'); return; }
        const fragment = document.createDocumentFragment();
        state.servers.forEach((server) => {
            const row = create('article', 'technical-row');
            row.dataset.techServer = server.id;
            const main = create('div', 'technical-row-main');
            main.append(create('h4', '', server.name), create('p', 'technical-meta', `${architectureLabels[server.architecture]} · ${serverRoleLabels[server.role]} · ${server.station}`), create('p', 'technical-meta technical-address', server.address), create('p', 'technical-meta', `Veritabanı profili: ${server.database || 'Atanmadı'} · Planlanan çevrimdışı kapsam: ${server.offlineMinutes} dk`));
            const actions = create('div', 'technical-actions');
            actions.append(actionButton('Düzenle', 'edit-server', server.id, `${server.name} sunucusunu düzenle`), actionButton('Sil', 'remove-server', server.id, `${server.name} sunucusunu sil`));
            row.append(main, actions);
            fragment.append(row);
        });
        host.replaceChildren(fragment);
    };
    const targetFor = (person) => person.scope === 'station' ? { id: person.station, label: person.station } : targets[person.scope]?.find((target) => target.id === person.target);
    const dateText = (value) => value ? new Date(value).toLocaleString('tr-TR', { dateStyle: 'short', timeStyle: 'short' }) : 'Belirtilmedi';
    const renderPeople = () => {
        const host = q('[data-tech-person-list]');
        if (!state.people.length) { empty(host, 'Yayın kişisi atanmadı'); return; }
        const fragment = document.createDocumentFragment();
        state.people.forEach((person) => {
            const row = create('article', 'technical-row');
            row.dataset.techPerson = person.id;
            const main = create('div', 'technical-row-main');
            const target = targetFor(person);
            const server = state.servers.find((entry) => entry.id === person.server);
            const role = person.role === 'custom' ? person.customRole : personRoleLabels[person.role];
            const permissions = Object.entries(person.permissions).filter(([, permitted]) => permitted).map(([name]) => accessLabels[name]).join(' · ') || 'İzin seçilmedi';
            main.append(create('h4', '', person.name), create('p', 'technical-meta', `${person.login} · ${role}`), create('p', 'technical-meta', `${scopeLabels[person.scope]}: ${target?.label || `${person.targetName || 'Önceki hedef'} · kaldırıldı / yeniden seçilmeli`}`), create('p', 'technical-meta', `Sunucu: ${server?.name || (person.serverRemoved ? 'Kaldırıldı · atanmadı' : 'Atanmadı')}`), create('p', 'technical-meta', `${person.period === 'temporary' ? 'Geçici' : 'Kalıcı'} · ${dateText(person.start)} → ${dateText(person.end)}`), create('p', 'technical-meta', `Yetkiler: ${permissions}`), ...(target ? [] : [create('span', 'technical-badge', 'Hedef yeniden seçilmeli')]));
            const actions = create('div', 'technical-actions');
            actions.append(actionButton('Düzenle', 'edit-person', person.id, `${person.name} atamasını düzenle`), actionButton('Sil', 'remove-person', person.id, `${person.name} atamasını sil`));
            row.append(main, actions);
            fragment.append(row);
        });
        host.replaceChildren(fragment);
    };
    const renderAudit = () => {
        const host = q('[data-tech-audit-list]');
        if (!state.audit.length) { host.replaceChildren(create('li', 'technical-meta', 'Kayıt yok')); return; }
        host.replaceChildren(...state.audit.map((entry) => {
            const row = create('li');
            const time = create('time', 'technical-meta', dateText(entry.at));
            time.dateTime = entry.at;
            row.append(time, create('span', '', entry.message));
            return row;
        }));
    };
    const readWeb = () => ({
        managementUrl: formText(webForm, 'managementUrl'), listenerUrl: formText(webForm, 'listenerUrl'), siteUrl: formText(webForm, 'siteUrl'), apiPath: formText(webForm, 'apiPath', 200), auth: field(webForm, 'auth').value, sessionMinutes: Number(field(webForm, 'sessionMinutes').value), permission: field(webForm, 'permission').value, siteSearch: checked(webForm, 'siteSearch'), siteRequests: checked(webForm, 'siteRequests'), siteAdverts: checked(webForm, 'siteAdverts'),
    });
    const renderWebPreview = (web = readWeb()) => {
        const managementUrl = urlValue(web.managementUrl, true);
        const listenerUrl = urlValue(web.listenerUrl);
        const siteUrl = urlValue(web.siteUrl, true);
        const path = safePath(web.apiPath);
        let endpoint = 'Tanımlanmadı';
        if (siteUrl && path) endpoint = new URL(path, siteUrl).href;
        else if (web.siteUrl || web.apiPath && web.apiPath !== webDefaults.apiPath) endpoint = 'Geçersiz adres';
        const values = { management: managementUrl || (web.managementUrl ? 'Geçersiz adres' : 'Tanımlanmadı'), listener: listenerUrl || (web.listenerUrl ? 'Geçersiz adres' : 'Tanımlanmadı'), endpoint, identity: `${authLabels[web.auth] || authLabels.password} · ${integer(web.sessionMinutes, 1, 1440, 30)} dk`, permission: permissionLabels[web.permission] || permissionLabels.monitor, features: [web.siteSearch ? 'Parça / albüm araması' : '', web.siteRequests ? 'İstek / iletişim formu' : '', web.siteAdverts ? 'Şifreli reklam teslimi' : ''].filter(Boolean).join(' · ') || 'Seçilmedi' };
        Object.entries(values).forEach(([name, value]) => { const output = q(`[data-tech-web-preview="${name}"]`); if (output) output.textContent = value; });
        const link = q('[data-tech-web-open]');
        if (managementUrl) { link.href = managementUrl; link.setAttribute('aria-disabled', 'false'); link.removeAttribute('tabindex'); }
        else { link.removeAttribute('href'); link.setAttribute('aria-disabled', 'true'); link.tabIndex = -1; }
    };
    const render = () => {
        for (const [key, count] of [['servers', state.servers.length], ['people', state.people.length]]) { const node = q(`[data-tech-summary="${key}"]`); if (node) node.textContent = String(count); }
        renderServers();
        renderPeople();
        renderAudit();
        renderWebPreview();
        renderServerOptions();
    };
    const renderServerOptions = (selected = field(personForm, 'server').value) => {
        const placeholder = create('option', '', 'Atanmadı');
        placeholder.value = '';
        const options = state.servers.map((server) => { const option = create('option', '', `${server.name} · ${server.station}`); option.value = server.id; return option; });
        field(personForm, 'server').replaceChildren(placeholder, ...options);
        field(personForm, 'server').value = state.servers.some((server) => server.id === selected) ? selected : '';
        const names = [...new Set(state.servers.map((server) => server.station))];
        q('#technical-station-suggestions').replaceChildren(...names.map((name) => { const option = create('option'); option.value = name; return option; }));
    };
    const renderTargetOptions = (selected = field(personForm, 'target').value) => {
        const scope = field(personForm, 'scope').value;
        const stationScope = scope === 'station';
        q('[data-tech-station-target-field]').hidden = !stationScope;
        q('[data-tech-dynamic-target-field]').hidden = stationScope;
        field(personForm, 'station').required = stationScope;
        field(personForm, 'station').disabled = !stationScope;
        const select = field(personForm, 'target');
        select.required = !stationScope;
        select.disabled = stationScope;
        const available = targets[scope] || [];
        const placeholder = create('option', '', available.length ? 'Hedef seç' : 'Hedef bulunamadı');
        placeholder.value = '';
        select.replaceChildren(placeholder, ...available.map((target) => { const option = create('option', '', target.label); option.value = target.id; return option; }));
        select.value = available.some((target) => target.id === selected) ? selected : '';
        select.setCustomValidity('');
    };
    const updatePersonFields = () => {
        const custom = field(personForm, 'role').value === 'custom';
        q('[data-tech-custom-role-field]').hidden = !custom;
        field(personForm, 'customRole').required = custom;
        field(personForm, 'customRole').disabled = !custom;
        field(personForm, 'end').required = field(personForm, 'period').value === 'temporary';
        renderTargetOptions();
    };
    const refreshTargets = () => {
        targets = { programme: [], player: [] };
        let programme = null;
        try { programme = window.RadioSProgramme?.getSnapshot?.()?.programme; } catch { /* An unavailable draft is shown as unavailable. */ }
        if (programme && typeof programme === 'object' && text(programme.name, 120)) targets.programme.push({ id: 'current-programme', label: text(programme.name, 120) });
        document.querySelectorAll('#players-workspace .lp-card[data-live-player]').forEach((player) => {
            const id = text(player.dataset.livePlayer, 200);
            if (id) targets.player.push({ id, label: text(player.querySelector('h3')?.textContent, 120) || id });
        });
        renderTargetOptions();
        renderPeople();
    };
    const openDialog = (dialog, origin) => {
        dialogFocus.set(dialog, origin || document.activeElement);
        dialog.showModal();
        dialog.querySelector('input:not([type="hidden"]):not(:disabled), select:not(:disabled)')?.focus();
    };
    const openServer = (server = null, origin = null) => {
        serverForm.reset();
        stripForm(serverForm);
        setFields(serverForm, server || { id: '', architecture: 'local', role: 'primary', offlineMinutes: 0 });
        q('#technical-server-dialog-title').textContent = server ? 'Sunucuyu düzenle' : 'Sunucu ekle';
        openDialog(q('#technical-server-dialog'), origin);
    };
    const openPerson = (person = null, origin = null) => {
        personForm.reset();
        stripForm(personForm);
        setFields(personForm, person ? { ...person, ...person.permissions } : { id: '', role: 'operator', scope: 'station', period: 'permanent', monitor: true });
        refreshTargets();
        renderServerOptions(person?.server || '');
        updatePersonFields();
        if (person) renderTargetOptions(person.target);
        q('#technical-person-dialog-title').textContent = person ? 'Yayın kişisini düzenle' : 'Yayın kişisi ata';
        openDialog(q('#technical-person-dialog'), origin);
    };
    root.querySelectorAll('.technical-dialog').forEach((dialog) => {
        dialog.addEventListener('close', () => { const origin = dialogFocus.get(dialog); if (origin?.isConnected && !origin.closest('[hidden]')) origin.focus({ preventScroll: true }); dialogFocus.delete(dialog); });
    });
    root.querySelectorAll('[data-tech-dialog-close]').forEach((button) => button.addEventListener('click', () => button.closest('dialog').close()));
    q('[data-tech-server-new]').addEventListener('click', (event) => openServer(null, event.currentTarget));
    q('[data-tech-person-new]').addEventListener('click', (event) => openPerson(null, event.currentTarget));
    root.addEventListener('click', (event) => {
        const button = event.target.closest('[data-tech-action]');
        if (!button || !root.contains(button)) return;
        const id = button.dataset.techId;
        const action = button.dataset.techAction;
        if (action === 'edit-server') openServer(state.servers.find((server) => server.id === id), button);
        if (action === 'edit-person') openPerson(state.people.find((person) => person.id === id), button);
        if (action === 'remove-server') {
            const server = state.servers.find((entry) => entry.id === id);
            if (!server) return;
            state.servers = state.servers.filter((entry) => entry.id !== id);
            let cleared = 0;
            state.people.forEach((person) => { if (person.server === id) { person.server = ''; person.serverRemoved = true; cleared += 1; } });
            save(`${server.name} sunucusu silindi.${cleared ? ` ${cleared} kişi atamasında sunucu kaldırıldı.` : ''}`);
            q('[data-tech-server-new]').focus({ preventScroll: true });
        }
        if (action === 'remove-person') {
            const person = state.people.find((entry) => entry.id === id);
            if (!person) return;
            state.people = state.people.filter((entry) => entry.id !== id);
            save(`${person.name} kişi ataması silindi.`);
            q('[data-tech-person-new]').focus({ preventScroll: true });
        }
    });
    serverForm.addEventListener('submit', (event) => {
        event.preventDefault();
        stripForm(serverForm);
        const address = validateUrl(serverForm, 'address', false);
        if (!serverForm.reportValidity()) return;
        const id = formText(serverForm, 'id');
        const existing = state.servers.find((server) => server.id === id);
        const server = { id: existing?.id || `technical-server-${++serverSequence}`, name: formText(serverForm, 'name', 100), architecture: field(serverForm, 'architecture').value, role: field(serverForm, 'role').value, station: formText(serverForm, 'station', 100), address, database: formText(serverForm, 'database', 100), offlineMinutes: Number(field(serverForm, 'offlineMinutes').value) };
        if (existing) Object.assign(existing, server);
        else state.servers.push(server);
        q('#technical-server-dialog').close();
        save(`${server.name} sunucusu ${existing ? 'güncellendi' : 'eklendi'}.`);
        q('[data-tech-server-new]').focus({ preventScroll: true });
    });
    ['role', 'scope', 'period'].forEach((name) => field(personForm, name).addEventListener('change', updatePersonFields));
    personForm.addEventListener('submit', (event) => {
        event.preventDefault();
        stripForm(personForm);
        const scope = field(personForm, 'scope').value;
        const selectedTarget = targets[scope]?.find((target) => target.id === field(personForm, 'target').value);
        if (scope !== 'station' && !selectedTarget) field(personForm, 'target').setCustomValidity('Mevcut bir program veya oynatıcı seç.');
        const start = field(personForm, 'start').value;
        const end = field(personForm, 'end').value;
        if (start && end && new Date(end) <= new Date(start)) field(personForm, 'end').setCustomValidity('Bitiş, başlangıçtan sonra olmalı.');
        if (!personForm.reportValidity()) return;
        const id = formText(personForm, 'id');
        const existing = state.people.find((person) => person.id === id);
        const role = field(personForm, 'role').value;
        const serverValue = field(personForm, 'server').value;
        const person = { id: existing?.id || `technical-person-${++personSequence}`, name: formText(personForm, 'name', 100), login: formText(personForm, 'login', 120), role, customRole: role === 'custom' ? formText(personForm, 'customRole', 100) : '', scope, station: scope === 'station' ? formText(personForm, 'station', 100) : '', target: scope === 'station' ? '' : selectedTarget.id, targetName: scope === 'station' ? formText(personForm, 'station', 100) : selectedTarget.label, server: state.servers.some((server) => server.id === serverValue) ? serverValue : '', serverRemoved: false, start, end, period: field(personForm, 'period').value, permissions: Object.fromEntries(Object.keys(accessLabels).map((name) => [name, checked(personForm, name)])) };
        if (existing) Object.assign(existing, person);
        else state.people.push(person);
        q('#technical-person-dialog').close();
        save(`${person.name} kişi ataması ${existing ? 'güncellendi' : 'eklendi'}.`);
        q('[data-tech-person-new]').focus({ preventScroll: true });
    });
    webForm.addEventListener('submit', (event) => {
        event.preventDefault();
        stripForm(webForm);
        const managementUrl = validateUrl(webForm, 'managementUrl', true);
        const listenerUrl = validateUrl(webForm, 'listenerUrl', false);
        const siteUrl = validateUrl(webForm, 'siteUrl', true);
        const apiPath = safePath(field(webForm, 'apiPath').value);
        if (!apiPath) field(webForm, 'apiPath').setCustomValidity('Bağlantı yolu tek / ile başlamalı ve boşluk içermemeli.');
        if ([checked(webForm, 'siteSearch'), checked(webForm, 'siteRequests'), checked(webForm, 'siteAdverts')].some(Boolean) && !siteUrl) field(webForm, 'siteUrl').setCustomValidity('Site işlevleri HTTPS site adresi gerektirir.');
        if (!webForm.reportValidity()) return;
        state.web = { ...readWeb(), managementUrl, listenerUrl, siteUrl, apiPath };
        setFields(webForm, state.web);
        save('Kaydedildi.');
    });
    root.addEventListener('input', (event) => {
        if ('setCustomValidity' in event.target) event.target.setCustomValidity('');
        if (personForm.contains(event.target)) field(personForm, 'end').setCustomValidity('');
        if (webForm.contains(event.target)) {
            [...webForm.elements].forEach((input) => { if ('setCustomValidity' in input) input.setCustomValidity(''); });
            renderWebPreview();
        }
    });
    webForm.addEventListener('change', () => renderWebPreview());
    q('[data-tech-web-open]').addEventListener('click', (event) => { if (!event.currentTarget.hasAttribute('href')) event.preventDefault(); });

    const tabs = [...root.querySelectorAll('[data-tech-tabs] [role="tab"]')];
    const activateTab = (selected, focus = false) => {
        tabs.forEach((tab) => {
            const active = tab === selected;
            tab.setAttribute('aria-selected', String(active));
            tab.tabIndex = active ? 0 : -1;
            const pane = document.getElementById(tab.getAttribute('aria-controls'));
            if (pane) pane.hidden = !active;
        });
        if (focus) selected.focus();
    };
    tabs.forEach((tab) => {
        tab.addEventListener('click', () => activateTab(tab));
        tab.addEventListener('keydown', (event) => {
            const index = tabs.indexOf(tab);
            const next = event.key === 'ArrowRight' ? (index + 1) % tabs.length : event.key === 'ArrowLeft' ? (index - 1 + tabs.length) % tabs.length : event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : null;
            if (next === null) return;
            event.preventDefault();
            activateTab(tabs[next], true);
        });
    });
    setFields(webForm, state.web);
    updatePersonFields();
    refreshTargets();
    render();
    window.RadioSTechnicalManagement = Object.freeze({ snapshot });
    ['radios:programme-change', 'radios:playlist-change', 'radios:live-player-change'].forEach((eventName) => document.addEventListener(eventName, refreshTargets));
    document.addEventListener('radios:profile-change', (event) => {
        if (event.detail?.profile === 'technical') return;
        root.querySelectorAll('.technical-dialog[open]').forEach((dialog) => dialog.close());
    });
    const playerList = document.querySelector('#players-workspace [data-live-player-list]');
    if (playerList) new MutationObserver(refreshTargets).observe(playerList, { childList: true });
})();
