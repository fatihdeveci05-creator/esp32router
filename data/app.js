// Tab Degistirme Fonksiyonu
function switchTab(tabId) {
    document.querySelectorAll('.tab-btn').forEach(btn => btn.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(content => content.classList.remove('active'));

    const activeBtn = Array.from(document.querySelectorAll('.tab-btn')).find(b => b.getAttribute('onclick').includes(tabId));
    if (activeBtn) activeBtn.classList.add('active');

    const activeContent = document.getElementById(tabId);
    if (activeContent) activeContent.classList.add('active');

    if (tabId === 'tab-clients') loadClients();
    if (tabId === 'tab-logs') loadLogs();
    if (tabId === 'tab-security') loadBlacklist();
}

// Bayt Formatlama Yardimcisi (KB, MB, GB)
function formatBytes(bytes) {
    if (!bytes || bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

// Sure Formatlama (Saniye -> Saat:Dk:Sn)
function formatUptime(seconds) {
    const d = Math.floor(seconds / (3600*24));
    const h = Math.floor((seconds % (3600*24)) / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);
    if (d > 0) return `${d}g ${h}s ${m}d`;
    if (h > 0) return `${h}s ${m}d ${s}sn`;
    return `${m}d ${s}sn`;
}

// 1. Genel Durum Bilgisini Cek
async function updateStatus() {
    try {
        const res = await fetch('/api/status');
        if (!res.ok) return;
        const data = await res.json();

        // Metrikleri Guncelle
        document.getElementById('val-total-rx').innerText = formatBytes(data.total_rx);
        document.getElementById('val-total-tx').innerText = formatBytes(data.total_tx);
        document.getElementById('val-free-heap').innerText = Math.round(data.free_heap / 1024) + ' KB';
        document.getElementById('val-uptime').innerText = formatUptime(data.uptime_sec);
        document.getElementById('client-count').innerText = data.connected_clients || 0;

        // Baglanti Durumu
        const statusBadge = document.getElementById('system-status-badge');
        const staStatus = document.getElementById('val-sta-status');
        const staSsid = document.getElementById('val-sta-ssid');
        const staRssi = document.getElementById('val-sta-rssi');

        if (data.sta_connected && data.napt_active) {
            statusBadge.innerHTML = '<span class="badge badge-success"><span class="status-dot dot-green"></span>İnternet Aktif</span>';
            staStatus.innerHTML = '<span style="color: var(--success);">Bağlı & Yönlendiriliyor</span>';
            staSsid.innerText = 'SSID: ' + (data.sta_ssid || '-');
            staRssi.innerText = data.sta_rssi + ' dBm';
        } else if (data.sta_ssid && data.sta_ssid.length > 0) {
            statusBadge.innerHTML = '<span class="badge badge-warning">Modeme Bağlanılıyor...</span>';
            staStatus.innerHTML = '<span style="color: var(--warning);">Bağlantı Aranıyor</span>';
            staSsid.innerText = 'Hedef: ' + data.sta_ssid;
            staRssi.innerText = '-';
        } else {
            statusBadge.innerHTML = '<span class="badge badge-danger">Modem Yapılandırılmadı</span>';
            staStatus.innerHTML = '<span style="color: var(--danger);">Yalnızca Yerel Ağ</span>';
            staSsid.innerText = 'Ayar sekmesinden Wi-Fi seçin';
            staRssi.innerText = '-';
        }
    } catch (err) {
        console.warn('Durum alinamadi:', err);
    }
}

// 2. Bagli Cihazlar Listesi
async function loadClients() {
    try {
        const res = await fetch('/api/clients');
        const clients = await res.json();
        const tbody = document.getElementById('clients-table-body');
        tbody.innerHTML = '';

        if (!clients || clients.length === 0) {
            tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: var(--text-muted);">Henüz bağlı cihaz tespit edilmedi.</td></tr>';
            return;
        }

        clients.forEach(c => {
            const tr = document.createElement('tr');
            const statusHtml = c.is_blocked 
                ? '<span class="badge badge-danger">Yasaklandı</span>' 
                : (c.active ? '<span class="badge badge-success">Aktif</span>' : '<span class="badge badge-warning">Boşta</span>');

            const actionBtn = c.is_blocked
                ? `<button class="btn btn-outline" onclick="toggleBlockClient('${c.mac}', false)">Engeli Kaldır</button>`
                : `<button class="btn btn-danger" onclick="toggleBlockClient('${c.mac}', true)">İnterneti Kes</button>`;

            tr.innerHTML = `
                <td><strong>${c.ip}</strong></td>
                <td><code>${c.mac || 'N/A'}</code></td>
                <td>${formatBytes(c.rx)}</td>
                <td>${formatBytes(c.tx)}</td>
                <td>${statusHtml}</td>
                <td>${actionBtn}</td>
            `;
            tbody.appendChild(tr);
        });
    } catch (err) {
        console.error('Cihazlar alinamadi:', err);
    }
}

// Cihaz Engelleme / Kaldirma
async function toggleBlockClient(mac, block) {
    if (!confirm(block ? 'Bu cihazın internet erişimini kesmek istiyor musunuz?' : 'Bu cihazın yasağını kaldırmak istiyor musunuz?')) return;
    try {
        const res = await fetch('/api/block-client', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mac, block })
        });
        if (res.ok) {
            loadClients();
        }
    } catch (err) {
        alert('İşlem başarısız: ' + err);
    }
}

// 3. Canli DNS ve Trafik Kayitlari
async function loadLogs() {
    try {
        const res = await fetch('/api/dns-logs');
        const logs = await res.json();
        const tbody = document.getElementById('logs-table-body');
        tbody.innerHTML = '';

        document.getElementById('log-count-badge').innerText = (logs.length || 0) + ' Kayıt';

        if (!logs || logs.length === 0) {
            tbody.innerHTML = '<tr><td colspan="4" style="text-align: center; color: var(--text-muted);">Henüz bir trafik kaydı oluşmadı.</td></tr>';
            return;
        }

        logs.forEach(log => {
            let actionBadge = '';
            if (log.action === 0) {
                actionBadge = '<span class="badge badge-success">İzin Verildi</span>';
            } else if (log.action === 1) {
                actionBadge = '<span class="badge badge-danger">Engellendi (0.0.0.0)</span>';
            } else if (log.action === 2) {
                actionBadge = '<span class="badge badge-primary">Yerel Site (denemesitem.com)</span>';
            }

            const tr = document.createElement('tr');
            tr.innerHTML = `
                <td style="color: var(--text-muted);">${formatUptime(log.time)}</td>
                <td><code>${log.ip}</code></td>
                <td><strong>${log.domain}</strong></td>
                <td>${actionBadge}</td>
            `;
            tbody.appendChild(tr);
        });
    } catch (err) {
        console.error('Loglar alinamadi:', err);
    }
}

// 4. Guvenlik & Kara Liste (Blacklist)
async function loadBlacklist() {
    try {
        const res = await fetch('/api/blacklist');
        const list = await res.json();
        const tbody = document.getElementById('blacklist-table-body');
        tbody.innerHTML = '';

        if (!list || list.length === 0) {
            tbody.innerHTML = '<tr><td colspan="3" style="text-align: center; color: var(--text-muted);">Kara liste boş.</td></tr>';
            return;
        }

        list.forEach((domain, idx) => {
            const tr = document.createElement('tr');
            tr.innerHTML = `
                <td>${idx + 1}</td>
                <td><strong>${domain}</strong></td>
                <td><button class="btn btn-outline" style="color: var(--danger);" onclick="removeBlacklistDomain('${domain}')">Kaldır</button></td>
            `;
            tbody.appendChild(tr);
        });
    } catch (err) {
        console.error('Blacklist alinamadi:', err);
    }
}

async function addBlacklistDomain() {
    const input = document.getElementById('new-domain-input');
    const domain = input.value.trim().toLowerCase();
    if (!domain) return;

    try {
        const res = await fetch('/api/blacklist', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'add', domain })
        });
        if (res.ok) {
            input.value = '';
            loadBlacklist();
        } else {
            alert('Domain eklenemedi!');
        }
    } catch (err) {
        alert('Hata: ' + err);
    }
}

async function removeBlacklistDomain(domain) {
    if (!confirm(`'${domain}' alan adını engelli listesinden çıkarmak istiyor musunuz?`)) return;
    try {
        const res = await fetch('/api/blacklist', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'remove', domain })
        });
        if (res.ok) {
            loadBlacklist();
        }
    } catch (err) {
        alert('Hata: ' + err);
    }
}

// 5. Wi-Fi Tarama ve Ayarlar
async function scanWifiNetworks() {
    const select = document.getElementById('wifi-select');
    select.innerHTML = '<option value="">Ağlar taranıyor, lütfen bekleyin...</option>';

    try {
        const res = await fetch('/api/wifi-scan');
        const nets = await res.json();

        select.innerHTML = '<option value="">-- Listeden bir ağ seçin --</option>';
        if (Array.isArray(nets)) {
            nets.forEach(n => {
                const opt = document.createElement('option');
                opt.value = n.ssid;
                opt.innerText = `${n.ssid} (${n.rssi} dBm) ${n.secure ? '🔒' : '🔓'}`;
                select.appendChild(opt);
            });
        }
    } catch (err) {
        select.innerHTML = '<option value="">Tarama hatası, tekrar deneyin.</option>';
    }
}

function onWifiSelectChange() {
    const select = document.getElementById('wifi-select');
    if (select.value) {
        document.getElementById('sta-ssid-input').value = select.value;
    }
}

async function saveWifiConfig() {
    const ssid = document.getElementById('sta-ssid-input').value.trim();
    const pass = document.getElementById('sta-pass-input').value;

    if (!ssid) {
        alert('Lütfen bir Wi-Fi SSID adı girin.');
        return;
    }

    try {
        const res = await fetch('/api/wifi-config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ ssid, pass })
        });
        if (res.ok) {
            alert('Ayarlar kaydedildi! ESP32 modeme bağlanmayı deniyor. Durumu Genel Bakış sekmesinden takip edebilirsiniz.');
            switchTab('tab-overview');
        }
    } catch (err) {
        alert('Kayıt hatası: ' + err);
    }
}

async function rebootEsp32() {
    if (!confirm('ESP32 yeniden başlatılacak. Emin misiniz?')) return;
    try {
        await fetch('/api/reboot', { method: 'POST' });
        alert('Cihaz yeniden başlatılıyor... Lütfen yaklaşık 15 saniye bekleyin.');
    } catch (err) {
        console.log(err);
    }
}

// Sayfa acilisinda calisacak dongu
window.addEventListener('DOMContentLoaded', () => {
    updateStatus();
    setInterval(updateStatus, 2500); // 2.5 saniyede bir durumlari tazele
});
