/**
 * LightNVR Web Interface Authentication Module
 * Handles authentication for all pages
 */

// Check if authentication is required and redirect if needed
function checkAuthentication() {
    // Skip authentication check for login page
    if (window.location.pathname.endsWith('login.html')) {
        return;
    }
    
    // Get stored credentials
    const auth = localStorage.getItem('auth');
    
    // Make a test request to check if authentication is required
    fetch('/api/settings', {
        headers: auth ? {
            'Authorization': 'Basic ' + auth
        } : {}
    })
    .then(response => {
        if (response.status === 401) {
            // Authentication required but no credentials or invalid credentials
            window.location.href = 'login.html';
        }
    })
    .catch(error => {
        console.error('Authentication check failed:', error);
        // Continue anyway in case of network error
    });
}

// Add authentication header to all fetch requests
function setupAuthInterceptor() {
    const originalFetch = window.fetch;
    
    window.fetch = function(url, options = {}) {
        const auth = localStorage.getItem('auth');
        
        if (auth) {
            options.headers = options.headers || {};
            options.headers['Authorization'] = 'Basic ' + auth;
        }
        
        return originalFetch(url, options)
            .then(response => {
                // If we get a 401, redirect to login page
                if (response.status === 401 && !window.location.pathname.endsWith('login.html')) {
                    window.location.href = 'login.html';
                }
                return response;
            });
    };
}

// Handle logout
function logout() {
    localStorage.removeItem('auth');
    window.location.href = 'index.html?logout=true';
}

// Initialize authentication
document.addEventListener('DOMContentLoaded', function() {
    checkAuthentication();
    setupAuthInterceptor();
    
    // Add event listener to logout button if it exists
    const logoutButton = document.getElementById('logout');
    if (logoutButton) {
        logoutButton.addEventListener('click', function(e) {
            e.preventDefault();
            logout();
        });
    }
});
