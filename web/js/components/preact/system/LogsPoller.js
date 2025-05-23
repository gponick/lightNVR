/**
 * LogsPoller Component
 * Handles polling for logs via WebSocket instead of automatic updates
 */

import { h } from '../../../preact.min.js';
import { html } from '../../../html-helper.js';
import { useState, useEffect, useRef } from '../../../preact.hooks.module.js';
import { log_level_meets_minimum } from './SystemUtils.js';
import { fetchJSON } from '../../../fetch-utils.js';

/**
 * LogsPoller component
 * @param {Object} props Component props
 * @param {string} props.logLevel Current log level filter
 * @param {number} props.logCount Number of logs to display
 * @param {Function} props.onLogsReceived Callback function when logs are received
 * @returns {JSX.Element} LogsPoller component (invisible)
 */
export function LogsPoller({ logLevel, logCount, onLogsReceived }) {
  const [isPolling, setIsPolling] = useState(false);
  const pollingIntervalRef = useRef(null);
  // Initialize with null, but will persist between renders
  const lastTimestampRef = useRef(null);
  
  // Try to load the last timestamp from localStorage on initial render
  useEffect(() => {
    const savedTimestamp = localStorage.getItem('lastLogTimestamp');
    if (savedTimestamp) {
      console.log('Loaded last log timestamp from localStorage:', savedTimestamp);
      lastTimestampRef.current = savedTimestamp;
    }
  }, []);
  
  // Function to fetch logs via WebSocket
  const fetchLogs = () => {
    if (!window.wsClient || !window.wsClient.isConnected()) {
      console.log('WebSocket not connected, skipping log fetch');
      return;
    }
    
    // Only fetch if we're on the system page
    if (!document.getElementById('system-page')) {
      console.log('Not on system page, skipping log fetch');
      return;
    }
    
    console.log('Fetching logs via WebSocket with level: debug (to get all logs, will filter on frontend)');
    
    // Create fetch request payload
    const payload = {
      level: 'debug', // Always request debug level to get all logs
      count: logCount
    };
    
    // Add last timestamp if available for pagination
    if (lastTimestampRef.current) {
      payload.last_timestamp = lastTimestampRef.current;
    }
    
    // Add client ID to the payload
    if (window.wsClient.getClientId) {
      payload.client_id = window.wsClient.getClientId();
    }
    
    console.log('Sending fetch request with payload:', payload);
    
    // Send fetch request
    try {
      // Always use the send method which properly formats the message
      const success = window.wsClient.send('fetch', 'system/logs', payload);
      if (!success) {
        console.warn('Failed to send fetch request, will retry on next poll');
      } else {
        console.log('Fetch request sent successfully');
      }
    } catch (error) {
      console.error('Error sending fetch request:', error);
    }
  };
  
  // Set up WebSocket handler for log updates - only once on mount
  useEffect(() => {
    if (!window.wsClient) {
      console.log('WebSocket client not available');
      return;
    }
    
    // Handler for log updates
    const handleLogsUpdate = (payload) => {
      console.log('Received logs update via WebSocket:', payload);
      
      // Only process updates if we're on the system page
      if (!document.getElementById('system-page')) {
        console.log('Not on system page, ignoring log update');
        return;
      }
      
      if (payload && payload.logs && Array.isArray(payload.logs)) {
        // Clean and normalize logs
        const cleanedLogs = payload.logs.map(log => {
          // Simply use the component attributes directly
          const normalizedLog = {
            timestamp: log.timestamp || 'Unknown',
            level: log.level || 'info',
            message: log.message || ''
          };
          
          // Convert level to lowercase for consistency
          if (normalizedLog.level) {
            normalizedLog.level = normalizedLog.level.toLowerCase();
          }
          
          // Normalize 'warn' to 'warning'
          if (normalizedLog.level === 'warn') {
            normalizedLog.level = 'warning';
          }
          
          return normalizedLog;
        });
        
        // Don't filter logs here - let the parent component handle filtering
        // This ensures we're always using the most current logLevel value
        
        // Update last timestamp for pagination if available
        if (payload.latest_timestamp) {
          lastTimestampRef.current = payload.latest_timestamp;
          // Save to localStorage for persistence between page refreshes
          localStorage.setItem('lastLogTimestamp', payload.latest_timestamp);
          console.log('Updated and saved last log timestamp:', payload.latest_timestamp);
        }
        
        // Call the callback with all logs - parent will filter
        if (cleanedLogs.length > 0) {
          console.log(`Received ${cleanedLogs.length} logs via WebSocket`);
          
          // Get existing logs from the parent component using enhanced fetch
          fetchJSON('/api/system/logs?level=debug&count=100', {
            timeout: 15000, // 15 second timeout
            retries: 1,     // Retry once
            retryDelay: 1000 // 1 second between retries
          })
            .then(data => {
              if (data.logs && Array.isArray(data.logs)) {
                // Combine existing logs with new logs
                const existingLogs = data.logs.map(log => {
                  // Normalize existing logs
                  const normalizedLog = {
                    timestamp: log.timestamp || 'Unknown',
                    level: (log.level || 'info').toLowerCase(),
                    message: log.message || ''
                  };
                  
                  // Normalize 'warn' to 'warning'
                  if (normalizedLog.level === 'warn') {
                    normalizedLog.level = 'warning';
                  }
                  
                  return normalizedLog;
                });
                
                // Combine existing logs with new logs, avoiding duplicates
                const combinedLogs = [...existingLogs];
                
                // Add new logs that don't already exist
                cleanedLogs.forEach(newLog => {
                  // Check if this log already exists
                  const exists = combinedLogs.some(existingLog => 
                    existingLog.timestamp === newLog.timestamp && 
                    existingLog.message === newLog.message
                  );
                  
                  if (!exists) {
                    combinedLogs.push(newLog);
                  }
                });
                
                // Sort logs by timestamp (newest first)
                combinedLogs.sort((a, b) => {
                  return new Date(b.timestamp) - new Date(a.timestamp);
                });
                
                // Call the callback with combined logs - don't filter here
                // This ensures WebSocket debug logs are included when debug is selected in UI
                onLogsReceived(combinedLogs);
              } else {
                // If no existing logs, just use the new logs
                onLogsReceived(cleanedLogs);
              }
            })
            .catch(error => {
              console.error('Error fetching existing logs:', error);
              // If error fetching existing logs, just use the new logs
              onLogsReceived(cleanedLogs);
            });
        } else {
          console.log('No logs received via WebSocket');
        }
      }
    };
    
    // Register handler for system logs updates - only once
    console.log('Registering handler for system/logs via WebSocket (once on mount)');
    window.wsClient.on('update', 'system/logs', handleLogsUpdate);
    
    // Clean up on unmount
    return () => {
      // Unregister handler
      console.log('Unregistering handler for system/logs via WebSocket (component unmounting)');
      window.wsClient.off('update', 'system/logs');
      
      // Clear polling interval
      if (pollingIntervalRef.current) {
        clearInterval(pollingIntervalRef.current);
        pollingIntervalRef.current = null;
      }
    };
  }, []); // Empty dependency array - only run once on mount
  
  // Start/stop polling when isPolling changes
  useEffect(() => {
    // Start polling
    if (isPolling && !pollingIntervalRef.current) {
      console.log('Starting log polling');
      
      // Subscribe to system logs topic
      if (window.wsClient && typeof window.wsClient.subscribe === 'function') {
        console.log('Subscribing to system/logs via WebSocket for polling');
        // Include the last timestamp in the subscription if available
        const subscriptionParams = { 
          level: 'debug',
          ...(lastTimestampRef.current ? { since: lastTimestampRef.current } : {})
        };
        window.wsClient.subscribe('system/logs', subscriptionParams);
        console.log(`Subscribed to system/logs with level: debug and last_timestamp: ${lastTimestampRef.current || 'NULL'}`);
      }
      
      // Fetch logs immediately
      fetchLogs();
      
      // Set up polling interval (every 5 seconds)
      console.log('Setting up polling interval for logs (every 5 seconds)');
      pollingIntervalRef.current = setInterval(() => {
        console.log('Polling interval triggered, fetching logs...');
        fetchLogs();
      }, 5000);
    }
    // Stop polling
    else if (!isPolling && pollingIntervalRef.current) {
      console.log('Stopping log polling');
      
      // Unsubscribe from system logs topic
      if (window.wsClient && typeof window.wsClient.unsubscribe === 'function') {
        console.log('Unsubscribing from system/logs via WebSocket');
        window.wsClient.unsubscribe('system/logs');
      }
      
      clearInterval(pollingIntervalRef.current);
      pollingIntervalRef.current = null;
    }
    
    // Clean up on unmount
    return () => {
      if (pollingIntervalRef.current) {
        clearInterval(pollingIntervalRef.current);
        pollingIntervalRef.current = null;
      }
      
      // Unsubscribe from system logs topic
      if (window.wsClient && typeof window.wsClient.unsubscribe === 'function') {
        console.log('Unsubscribing from system/logs via WebSocket on cleanup');
        window.wsClient.unsubscribe('system/logs');
      }
    };
  }, [isPolling, logLevel]);
  
  // Start polling when component mounts and update when log level changes
  useEffect(() => {
    console.log(`LogsPoller: Setting up polling with log level ${logLevel}`);
    setIsPolling(false); // Stop any existing polling
    
    // Small delay to ensure any previous polling is cleaned up
    setTimeout(() => {
      setIsPolling(true); // Start polling with new parameters
    }, 100);
    
    // Clean up on unmount
    return () => {
      console.log('LogsPoller: Cleaning up on unmount');
      setIsPolling(false);
    };
  }, [logLevel, logCount]);
  
  // This component doesn't render anything visible
  return null;
}
