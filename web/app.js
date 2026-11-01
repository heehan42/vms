// 서버가 HTML과 별도 스크립트 파일을 모두 전달했는지 화면에서 확인할 수 있게 한다.
document.getElementById("script-status").textContent = "app.js가 실행되었습니다.";

const videoSelect = document.getElementById("video-select");
const connectButton = document.getElementById("connect-button");
const signalingStatus = document.getElementById("signaling-status");
const iceState = document.getElementById("ice-state");
const connectionState = document.getElementById("connection-state");
const dtlsState = document.getElementById("dtls-state");
const srtpCipher = document.getElementById("srtp-cipher");
const remoteVideo = document.getElementById("remote-video");

// 연결 버튼을 다시 누르면 이전 연결을 닫고 새로 시작하기 위해 보관한다.
let peerConnection = null;

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
function showVideoSelectMessage(message) {
    const option = document.createElement("option");
    option.value = "";
    option.textContent = message;
    videoSelect.replaceChildren(option);
    videoSelect.disabled = true;
}

// 서버의 동영상 목록을 받아 select 요소를 채운다. 첫 동영상이 선택된 상태가 된다.
//
// 동영상 ID는 파일 이름이라 HTML로 해석되면 안 되므로 textContent로만 넣는다.
async function loadVideoList() {
    const response = await fetch("/api/videos");
    if (!response.ok) {
        throw new Error(`${response.status} ${response.statusText}`);
    }
    const {videos} = await response.json();
    if (videos.length === 0) {
        showVideoSelectMessage("동영상 없음");
        return;
    }
    const options = videos.map((video) => {
        const option = document.createElement("option");
        option.value = video.id;
        option.textContent = video.id;
        return option;
    });
    videoSelect.replaceChildren(...options);
    videoSelect.disabled = false;
}

// 수신 전용 비디오 트랜시버로 offer를 만들어 서버에 보내고, answer를 적용해 ICE 연결을 시작한다.
//
// select 요소에서 고른 동영상 ID를 video 쿼리 매개변수로 붙인다. 고른 값이 없으면(목록이 비었거나 받지
// 못함) 붙이지 않으며, 서버는 동영상 없이 연결만 맺는다.
//
// 서버는 ICE-lite라 브라우저 candidate를 쓰지 않으므로, 후보 수집을 기다리지 않고 offer를 바로 보낸다.
async function connect() {
    if (peerConnection !== null) {
        peerConnection.close();
    }
    const connection = new RTCPeerConnection();
    peerConnection = connection;
    dtlsState.textContent = "-";
    srtpCipher.textContent = "-";
    // 이전 연결의 마지막 화면이 새 연결의 영상처럼 보이지 않도록 비운다.
    remoteVideo.srcObject = null;

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
    const videoId = videoSelect.value;
    const offerUrl = videoId ? `/offer?video=${encodeURIComponent(videoId)}` : "/offer";
    const response = await fetch(offerUrl, {
        method: "POST",
        headers: {"Content-Type": "application/sdp"},
        body: connection.localDescription.sdp,
    });
    if (!response.ok) {
        signalingStatus.textContent = `실패: ${response.status} ${response.statusText}`;
        return;
    }

    await connection.setRemoteDescription({type: "answer", sdp: await response.text()});
    signalingStatus.textContent = "answer 적용 완료";
    showStates();
}

// 목록이 비었거나 요청이 실패해도 동영상 없이 연결을 확인할 수 있어야 하므로, 끝나면 항상 버튼을 켠다.
loadVideoList()
    .catch((error) => {
        showVideoSelectMessage(`목록을 불러오지 못함: ${error.message}`);
    })
    .finally(() => {
        connectButton.disabled = false;
    });

connectButton.addEventListener("click", () => {
    connect().catch((error) => {
        signalingStatus.textContent = `오류: ${error.message}`;
    });
});
