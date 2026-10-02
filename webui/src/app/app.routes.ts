import { Routes } from '@angular/router';

// Served by dsd-server under /ui/ (DSD_WEBUI_DIR): /ui/ is the status page,
// /ui/net the network explorer.
export const routes: Routes = [
  { path: '', title: 'dsd-server status', loadComponent: () => import('./status/status-page').then((m) => m.StatusPage) },
  { path: 'net', title: 'dsd-server network explorer', loadComponent: () => import('./net/net-page').then((m) => m.NetPage) },
  { path: '**', redirectTo: '' },
];
