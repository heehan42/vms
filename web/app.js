// 서버가 HTML과 별도 스크립트 파일을 모두 전달했는지 화면에서 확인할 수 있게 한다.
document.getElementById("script-status").textContent = "app.js가 실행되었습니다.";

const connectButton = document.getElementById("connect-button");
const signalingStatus = document.getElementById("signaling-status");
const iceState = document.getElementById("ice-state");
const connectionState = document.getElementById("connection-state");
const dtlsState = document.getElementById("dtls-state");
const srtpCipher = document.getElementById("srtp-cipher");

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

// 수신 전용 비디오 트랜시버로 offer를 만들어 서버에 보내고, answer를 적용해 ICE 연결을 시작한다.
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
    const response = await fetch("/offer", {
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

connectButton.addEventListener("click", () => {
    connect().catch((error) => {
        signalingStatus.textContent = `오류: ${error.message}`;
    });
});
