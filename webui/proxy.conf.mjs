// `npm start` (ng serve) proxies the server's endpoints to a running
// dsd-server: DSD_SERVER=http://host:port (default http://localhost:22600).
const target = process.env.DSD_SERVER || 'http://localhost:22600';
export default [{
  context: ['/status.json', '/log.json', '/log/', '/iq_log/', '/net.json', '/net/'],
  target,
  changeOrigin: true,
}];
