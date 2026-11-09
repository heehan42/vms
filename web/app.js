// 서버가 HTML과 별도 스크립트 파일을 모두 전달했는지 화면에서 확인할 수 있게 한다.
document.getElementById("script-status").textContent = "app.js가 실행되었습니다.";

const sourceSelect = document.getElementById("source-select");
const connectButton = document.getElementById("connect-button");
const signalingStatus = document.getElementById("signaling-status");
const iceState = document.getElementById("ice-state");
const connectionState = document.getElementById("connection-state");
const dtlsState = document.getElementById("dtls-state");
const srtpCipher = document.getElementById("srtp-cipher");
const remoteVideo = document.getElementById("remote-video");
const recordingStatus = document.getElementById("recording-status");
const recordingTime = document.getElementById("recording-time");

// 진행 중인 WebRTC 연결. 연결 버튼을 다시 누르면 이전 연결을 닫고 새로 시작하기 위해 보관한다.
//
// peerConnection과 hlsPlayer 중 많아야 하나만 null이 아니다. 연결 버튼을 누르면 stopPlayback()이 둘 다 정리한다.
let peerConnection = null;

// 진행 중인 녹화 재생의 hls.js 객체. 녹화를 재생하지 않으면 null이다.
let hlsPlayer = null;

// GET /api/sources로 받은 소스 배열. option 요소의 값은 이 배열의 인덱스다.
//
// 이름에 어떤 문자가 와도 종류와 이름을 따로 꺼낼 수 있도록, option 값에 둘을 이어 붙이지 않고 인덱스만 둔다.
// 페이지가 열릴 때 한 번만 채우므로 인덱스는 바뀌지 않는다.
let sources = [];

// 목록의 type 값별로 option 요소에 표시할 종류 이름
const sourceTypeNames = {file: "파일", camera: "카메라", recording: "녹화"};

// 연결의 transport 통계에서 DTLS 상태와 SRTP 암호를 읽어 화면에 표시한다.
//
// connection: 연결 상태가 connected가 된 RTCPeerConnection
async function showTransportStats(connection) {
    const stats = await connection.getStats();
    // 통계를 읽는 동안 연결 버튼을 다시 눌렀으면 이전 연결의 결과는 버린다.
    if (connection !== peerConnection) {
        return;
    }
    for (const report of stats.values()) {
        if (report.type === "transport") {
            dtlsState.textContent = report.dtlsState ?? "-";
            srtpCipher.textContent = report.srtpCipher ?? "-";
            return;
        }
    }
}

// select 요소의 option 요소를 모두 지우고, 값이 빈 안내 option 요소 하나만 둔 채 비활성화한다.
//
// message: 안내 option 요소에 표시할 문구
function showSourceSelectMessage(message) {
    const option = document.createElement("option");
    option.value = "";
    option.textContent = message;
    sourceSelect.replaceChildren(option);
    sourceSelect.disabled = true;
}

// 서버의 소스 목록을 받아 select 요소를 채운다. 첫 소스가 선택된 상태가 된다.
//
// 소스 이름은 파일 이름 등이라 HTML로 해석되면 안 되므로 textContent로만 넣는다.
async function loadSourceList() {
    const response = await fetch("/api/sources");
    if (!response.ok) {
        throw new Error(`${response.status} ${response.statusText}`);
    }
    ({sources} = await response.json());
    if (sources.length === 0) {
        showSourceSelectMessage("소스 없음");
        return;
    }
    const options = sources.map((source, index) => {
        const option = document.createElement("option");
        option.value = String(index);
        option.textContent = `${sourceTypeNames[source.type] ?? source.type} · ${source.name}`;
        return option;
    });
    sourceSelect.replaceChildren(...options);
    sourceSelect.disabled = false;
}

// 수신 전용 비디오 트랜시버로 offer를 만들어 서버에 보내고, answer를 적용해 ICE 연결을 시작한다.
//
// source: 고른 소스(type이 file 또는 camera). 소스의 type과 name을 같은 이름의 쿼리 매개변수로 붙인다.
//   undefined이면(목록이 비었거나 받지 못함) 붙이지 않으며, 서버는 소스 없이 연결만 맺는다.
//
// 서버는 ICE-lite라 브라우저 candidate를 쓰지 않으므로, 후보 수집을 기다리지 않고 offer를 바로 보낸다.
// 호출 전에 stopPlayback()으로 이전 재생을 정리해야 한다.
async function connectWebRtc(source) {
    const connection = new RTCPeerConnection();
    peerConnection = connection;

    // 받은 비디오 트랙을 표시한다. answer의 a=msid가 있으면 그 스트림을, 없으면 트랙만으로 만든 스트림을 쓴다.
    connection.addEventListener("track", (event) => {
        if (connection !== peerConnection) {
            return;
        }
        remoteVideo.srcObject = event.streams[0] ?? new MediaStream([event.track]);
    });

    // 상태가 바뀔 때마다 화면을 갱신한다. 이전 연결의 이벤트는 무시한다.
    const showStates = () => {
        if (connection !== peerConnection) {
            return;
        }
        iceState.textContent = connection.iceConnectionState;
        connectionState.textContent = connection.connectionState;
        if (connection.connectionState === "connected") {
            showTransportStats(connection).catch((error) => {
                dtlsState.textContent = `통계 오류: ${error.message}`;
            });
        }
    };
    connection.addEventListener("iceconnectionstatechange", showStates);
    connection.addEventListener("connectionstatechange", showStates);

    connection.addTransceiver("video", {direction: "recvonly"});
    await connection.setLocalDescription(await connection.createOffer());
    showStates();

    signalingStatus.textContent = "offer 전송 중";
    const offerUrl = source
        ? `/offer?type=${encodeURIComponent(source.type)}&name=${encodeURIComponent(source.name)}`
        : "/offer";
    const response = await fetch(offerUrl, {
        method: "POST",
        headers: {"Content-Type": "application/sdp"},
        body: connection.localDescription.sdp,
    });
    // 응답을 기다리는 동안 연결 버튼을 다시 눌렀으면 이전 연결의 결과는 버린다.
    if (connection !== peerConnection) {
        return;
    }
    if (!response.ok) {
        signalingStatus.textContent = `실패: ${response.status} ${response.statusText}`;
        return;
    }

    await connection.setRemoteDescription({type: "answer", sdp: await response.text()});
    signalingStatus.textContent = "answer 적용 완료";
    showStates();
}

// 진행 중인 WebRTC 연결이나 녹화 재생을 끝내고 화면의 영상, 재생 막대, 상태 표시를 비운다.
//
// 이전 재생의 마지막 화면이 새 재생의 영상처럼 보이지 않도록 <video>의 srcObject와 src를 모두 비운다.
function stopPlayback() {
    if (peerConnection !== null) {
        peerConnection.close();
        peerConnection = null;
    }
    if (hlsPlayer !== null) {
        hlsPlayer.destroy();
        hlsPlayer = null;
    }
    remoteVideo.srcObject = null;
    remoteVideo.removeAttribute("src");
    remoteVideo.load();
    remoteVideo.controls = false;

    for (const element of [signalingStatus, iceState, connectionState, dtlsState, srtpCipher, recordingStatus,
                           recordingTime]) {
        element.textContent = "-";
    }
}

// 카메라 폴더 하나의 녹화 재생 목록을 hls.js로 <video>에 재생한다.
//
// source: 고른 소스(type이 recording). name은 카메라 폴더 이름이다.
//
// offer를 보내지 않으며, 재생하는 동안 재생 막대를 켠다. 재생 목록을 해석하면 처음부터 재생한다. hls.js가 복구할 수
// 없는 오류를 알리면(재생 중 보관 규칙으로 지워진 파일의 세그먼트 요청이 실패한 경우 등) 원인을 표시하고 재생을
// 멈춘다. 복구할 수 있는 오류는 hls.js가 다시 시도하므로 표시하지 않는다. 호출 전에 stopPlayback()으로 이전 재생을
// 정리해야 한다.
function startRecordingPlayback(source) {
    if (typeof Hls === "undefined" || !Hls.isSupported()) {
        recordingStatus.textContent = "오류: 이 브라우저는 hls.js(Media Source Extensions)를 지원하지 않습니다";
        return;
    }

    const player = new Hls();
    hlsPlayer = player;
    recordingStatus.textContent = "재생 목록 요청 중";
    player.on(Hls.Events.MANIFEST_PARSED, () => {
        if (player !== hlsPlayer) {
            return;
        }
        recordingStatus.textContent = "재생 중";
        remoteVideo.play().catch((error) => {
            recordingStatus.textContent = `재생 시작 실패: ${error.message}`;
        });
    });
    player.on(Hls.Events.ERROR, (event, data) => {
        if (player !== hlsPlayer || !data.fatal) {
            return;
        }
        recordingStatus.textContent = `오류: ${data.type} ${data.details}`;
        remoteVideo.pause();
        player.destroy();
        hlsPlayer = null;
    });

    remoteVideo.controls = true;
    player.loadSource(`/api/recordings/playlist.m3u8?camera=${encodeURIComponent(source.name)}`);
    player.attachMedia(remoteVideo);
}

// 재생 위치가 바뀔 때마다 지금 보이는 화면의 녹화 시각을 표시한다.
//
// hls.playingDate는 재생 위치에 해당하는 #EXT-X-PROGRAM-DATE-TIME 기준의 실제 시각(Date)이다. 파일 경계의
// #EXT-X-DISCONTINUITY를 지나면 다음 파일의 시작 시각을 기준으로 계산된다. 녹화를 재생하지 않으면 바꾸지 않는다.
remoteVideo.addEventListener("timeupdate", () => {
    const playingDate = hlsPlayer?.playingDate;
    if (playingDate) {
        recordingTime.textContent = playingDate.toLocaleString();
    }
});

// 이전 재생을 끝내고 select 요소에서 고른 소스를 재생한다.
//
// 고른 소스의 type이 recording이면 hls.js로 녹화를 재생하고, 아니면(file, camera, 소스 없음) WebRTC로 연결한다.
async function connect() {
    stopPlayback();
    const source = sourceSelect.value === "" ? undefined : sources[Number(sourceSelect.value)];
    if (source?.type === "recording") {
        startRecordingPlayback(source);
        return;
    }
    await connectWebRtc(source);
}

// 목록이 비었거나 요청이 실패해도 소스 없이 연결을 확인할 수 있어야 하므로, 끝나면 항상 버튼을 켠다.
loadSourceList()
    .catch((error) => {
        showSourceSelectMessage(`목록을 불러오지 못함: ${error.message}`);
    })
    .finally(() => {
        connectButton.disabled = false;
    });

connectButton.addEventListener("click", () => {
    connect().catch((error) => {
        signalingStatus.textContent = `오류: ${error.message}`;
    });
});
