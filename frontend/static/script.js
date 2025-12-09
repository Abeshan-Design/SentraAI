// Load stats on page load
document.addEventListener('DOMContentLoaded', () => {
    loadStats();

    // Allow Enter to send (Shift+Enter for newline)
    document.getElementById('question-input').addEventListener('keydown', (e) => {
        if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            sendQuestion();
        }
    });
});

async function loadStats() {
    try {
        const healthRes = await fetch('/api/health');
        const health = await healthRes.json();

        const statsRes = await fetch('/api/stats');
        const stats = await statsRes.json();

        document.getElementById('status').textContent = health.index_loaded ? 'Ready' : 'No Index';
        document.getElementById('status').style.color = health.index_loaded ? '#2ecc71' : '#e74c3c';
        document.getElementById('doc-count').textContent = stats.total_documents;
        document.getElementById('index-size').textContent = stats.index_size_mb + ' MB';

    } catch (error) {
        console.error('Failed to load stats:', error);
        document.getElementById('status').textContent = 'Error';
        document.getElementById('status').style.color = '#e74c3c';
    }
}

async function sendQuestion() {
    const input = document.getElementById('question-input');
    const question = input.value.trim();

    if (!question) return;

    // Add user message
    addMessage(question, 'user');
    input.value = '';

    // Disable input while processing
    const sendBtn = document.getElementById('send-btn');
    sendBtn.disabled = true;
    sendBtn.textContent = 'Thinking...';

    try {
        const response = await fetch('/api/query', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ question })
        });

        if (!response.ok) {
            throw new Error('Query failed');
        }

        const data = await response.json();
        addMessage(data.answer, 'bot');

    } catch (error) {
        addMessage('Error: ' + error.message, 'bot');
    } finally {
        sendBtn.disabled = false;
        sendBtn.textContent = 'Send';
    }
}

function addMessage(text, sender) {
    const messagesDiv = document.getElementById('messages');
    const messageDiv = document.createElement('div');
    messageDiv.className = `message ${sender}`;

    const contentDiv = document.createElement('div');
    contentDiv.className = 'message-content';

    if (sender === 'bot') {
        contentDiv.innerHTML = `<strong>SentraAI:</strong> ${text}`;
    } else {
        contentDiv.textContent = text;
    }

    messageDiv.appendChild(contentDiv);
    messagesDiv.appendChild(messageDiv);
    messagesDiv.scrollTop = messagesDiv.scrollHeight;
}

async function showDocuments() {
    const modal = document.getElementById('documents-modal');
    const listDiv = document.getElementById('documents-list');

    listDiv.innerHTML = '<div class="loading">Loading documents...</div>';
    modal.style.display = 'block';

    try {
        const response = await fetch('/api/documents?limit=100');
        const docs = await response.json();

        if (docs.length === 0) {
            listDiv.innerHTML = '<p>No documents indexed yet.</p>';
            return;
        }

        listDiv.innerHTML = docs.map(doc => `
            <div class="document-item">
                <h3>${doc.id}</h3>
                <div class="source">Source: ${doc.source}</div>
                <div class="preview">${doc.content_preview}</div>
            </div>
        `).join('');

    } catch (error) {
        listDiv.innerHTML = `<p style="color: red;">Error loading documents: ${error.message}</p>`;
    }
}

function closeModal() {
    document.getElementById('documents-modal').style.display = 'none';
}

async function triggerIngest() {
    if (!confirm('This will re-process all PDFs in data_raw/ and rebuild the index. Continue?')) {
        return;
    }

    const btn = event.target;
    btn.disabled = true;
    btn.textContent = 'Processing...';

    try {
        const response = await fetch('/api/ingest', { method: 'POST' });
        const data = await response.json();

        alert('Success: ' + data.message);
        loadStats();

    } catch (error) {
        alert('Ingestion failed: ' + error.message);
    } finally {
        btn.disabled = false;
        btn.textContent = 'Re-ingest PDFs';
    }
}

// Close modal when clicking outside
window.onclick = function(event) {
    const modal = document.getElementById('documents-modal');
    if (event.target === modal) {
        modal.style.display = 'none';
    }
}