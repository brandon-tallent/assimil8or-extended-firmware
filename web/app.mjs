// patchFirmware and zipFiles are embedded alongside this UI when packaged.

const input = document.querySelector('#firmware');
const status = document.querySelector('#status');
const download = document.querySelector('#download');
const MAX_PICK_BYTES = 4000000;
let outputUrl;

input.addEventListener('change', async () => {
  const file = input.files[0];
  if (!file) return;
  input.disabled = true;
  download.hidden = true;
  download.removeAttribute('href');
  if (outputUrl) URL.revokeObjectURL(outputUrl);
  status.textContent = 'Checking software…';
  status.dataset.error = 'false';
  try {
    if (!globalThis.crypto?.subtle) throw new Error('Use a current browser, or open the hosted patcher over HTTPS.');
    if (file.size > MAX_PICK_BYTES) throw new Error('Choose Rossum’s Assimil8or v3.04 download: the zip, or the app file inside it.');
    const bundle = JSON.parse(document.querySelector('#firmware-data').textContent);
    const result = await patchFirmware(new Uint8Array(await file.arrayBuffer()), bundle);
    const zip = zipFiles([{name: 'app', data: result.app}, {name: result.extensionName, data: result.extension}]);
    outputUrl = URL.createObjectURL(new Blob([zip], {type: 'application/zip'}));
    download.href = outputUrl;
    download.hidden = false;
    download.click();
    status.textContent = `Your software is ready. Unzip it and copy app and ${result.extensionName} to the top level of your micro SD card.`;
  } catch (error) {
    status.dataset.error = 'true';
    status.textContent = error instanceof Error ? error.message : 'Could not prepare the software. Please try again.';
  } finally {
    input.disabled = false;
    input.value = '';
  }
});
