#include "mpd/download/transportcontroller/transportcontroller.hpp"
#include "sessionstreamcontroller.hpp"

uint32_t kMaxIntervalSize = 3;
uint32_t kPacketReorderThreshold = 0;
float kProbeStepGain = 0.25;
float kUtilityEwmaAlpha = 0.5;
float kUtilityError = 0.2;
float kMaxLossRate = 0.20;
float kBackOffFactor = 0.7;

TestCongestionCtrl::TestCongestionCtrl(const TestCongestionCtlConfig& ccConfig)
{
    m_id = rand();
    SPDLOG_DEBUG("ccid:{}", m_id);
    m_bestCwnd = m_initCwnd;
    // create the first interval
    CreateInterval();
}

TestCongestionCtrl::~TestCongestionCtrl()
{
    SPDLOG_DEBUG("");
}

CongestionCtlType TestCongestionCtrl::GetCCtype()
{
    return CongestionCtlType::test;
}

void TestCongestionCtrl::OnDataSent(const InflightPacket& sentpkt)
{
    // create new interval
    if (MaybeCreateInterval()) {
        CreateInterval();
    }
    // update sent interval history
    if (!m_intervalHistory.back().sent_done) {
        m_intervalHistory.back().OnDataSent(sentpkt);
    }
}

void TestCongestionCtrl::OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats)
{
    if (lossEvent.valid)
    {
        //OnDataLoss(lossEvent);
    }
    if (ackEvent.valid)
    {
        OnDataRecv(ackEvent);
    }
    if (MaybeCreateInterval())
    {
        CreateInterval();
    }
    m_latestRtt = rttstats.latest_rtt();
}

void TestCongestionCtrl::OnDataLoss(const LossEvent& lossEvent)
{
    QuicTime now = Clock::GetClock()->Now();
    uint32_t maxLossSeq = 0;
    for (auto& itvl : m_intervalHistory)
    {
        for (auto &pkt : lossEvent.lossPackets)
        {
            maxLossSeq = std::max(maxLossSeq, pkt.seq);
            if (pkt.seq >= itvl.start_seq && pkt.seq <= itvl.end_seq)
            {
                if (!itvl.first_recv_tic.IsInitialized()) {
                    itvl.first_recv_tic = Clock::GetClock()->Now();
                }
                ++itvl.loss_cnt;
                if (itvl.send_cnt == itvl.loss_cnt + itvl.ack_cnt)
                {
                    itvl.recv_done = true;
                    itvl.CalculateStatistics();
                }
            }
        }
    }
    m_intervalHistory.back().CheckIfSentDone(maxLossSeq, now, 0);
}

void TestCongestionCtrl::OnDataRecv(const AckEvent& ackEvent)
{
    uint32_t newly_free = 0;
    if (m_largestAcked == MAX_SEQNUMBER)
    {
        m_largestAcked = ackEvent.ackPacket.seq;
    }
    else
    {
        uint32_t newly_largetst_acked = std::max(m_largestAcked, ackEvent.ackPacket.seq);
        newly_free = newly_largetst_acked - m_largestAcked;
        m_largestAcked = newly_largetst_acked;
    }
    m_freeCnt += newly_free;
    QuicTime now = Clock::GetClock()->Now();
    for (auto& itvl : m_intervalHistory)
    {
        if (ackEvent.ackPacket.seq >= itvl.start_seq + kPacketReorderThreshold && !itvl.first_recv_tic.IsInitialized())
        {
            itvl.first_recv_tic = now;
        }
        if (ackEvent.ackPacket.seq >= itvl.start_seq && ackEvent.ackPacket.seq <= itvl.end_seq)
        {
            itvl.avg_rtt = (itvl.avg_rtt * (float)itvl.ack_cnt + (now - ackEvent.sendtic)) * (1.0f / (itvl.ack_cnt + 1));
            ++itvl.ack_cnt;
            //itvl.ackTimeInMs.insert(now.ToDebuggingValue() / 1000);
        }
        if (itvl.sent_done && !itvl.recv_done && ackEvent.ackPacket.seq >= itvl.end_seq + kPacketReorderThreshold)
        {
            itvl.recv_done = true;
            itvl.CalculateStatistics();
            UpdateCwndProfile(itvl);
        }
    }
    m_intervalHistory.back().CheckIfSentDone(ackEvent.ackPacket.seq, now, m_freeCnt / m_aicnt);
    m_freeCnt = m_freeCnt % m_aicnt;
}

uint32_t TestCongestionCtrl::GetCWND()
{
    if (!m_intervalHistory.empty()) {
        return m_intervalHistory.back().output_cwnd;
    } else {
        return m_initCwnd;
    }
}

void TestCongestionCtrl::UpdateCwndProfile(Interval &itvl)
{
    auto itr = m_cwndStats.find(itvl.target_cwnd);
    if (itr == m_cwndStats.end())
    {
        CwndProfile profile;
        profile.cwnd = itvl.target_cwnd;
        profile.utility = itvl.utility;
        m_cwndStats[itvl.target_cwnd] = profile;
    }
    else
    {
        itr->second.utility = itr->second.utility * (1 - kUtilityEwmaAlpha) + itvl.utility * kUtilityEwmaAlpha;
    }

    if (itvl.type == IntervalType::NORMAL)
    {
        return;
    }

    //bool backoff = itvl.loss_rate > kMaxLossRate ||
    //                (m_cwndStats.find(m_bestCwnd) != m_cwndStats.end() && itvl.utility < m_cwndStats[m_bestCwnd].utility * (1 - kUtilityError));
    bool backoff = itvl.loss_rate > kMaxLossRate;
    uint32_t backoff_cwnd = BoundCwnd(itvl.target_cwnd * kBackOffFactor);

    if (backoff)
    {
        m_cwndStats.erase(m_cwndStats.lower_bound(backoff_cwnd + 1), m_cwndStats.end());
    }

    itr = m_cwndStats.begin();
    uint32_t best_cwnd = m_minCwnd;
    float best_utility = 0;

    for (auto &itr : m_cwndStats)
    {
        if (itr.second.utility > best_utility)
        {
            best_cwnd = itr.first;
            best_utility = itr.second.utility;
        }
    }

    m_cwndStats.erase(m_cwndStats.lower_bound(best_cwnd + 1), m_cwndStats.end());

    if (backoff)
    {
        m_probeStepPower = 0;
        while (best_cwnd + (1 << (m_probeStepPower + 1)) <= backoff_cwnd)
        {
            ++m_probeStepPower;
        }
    }
    else
    {
        uint32_t step = 1 << m_probeStepPower;
        if (step >= best_cwnd * kProbeStepGain && best_cwnd <= m_bestCwnd)
        {
            m_probeStepPower = 0;
        }
        else
        {
            ++m_probeStepPower;
            m_probeStepPower = std::min(m_probeStepPower, 31u);
        }
    }

    SPDLOG_DEBUG("m_bestCwnd:{} best_cwnd:{} backoff:{} m_probeStepPower:{}", m_bestCwnd, best_cwnd, backoff, m_probeStepPower);

    m_bestCwnd = best_cwnd;
}

bool TestCongestionCtrl::IsSeqValid(uint32_t seq)
{
    return seq != MAX_SEQNUMBER;
}

bool TestCongestionCtrl::MaybeCreateInterval()
{
    return m_intervalHistory.empty() || m_intervalHistory.back().sent_done;
}

void TestCongestionCtrl::CreateInterval()
{
    Interval itvl;
    itvl.conn_id = m_id;
    itvl.extra_duration = m_latestRtt * ((float)(rand()) / (float)(RAND_MAX));
    //itvl.extra_duration = QuicTime::Delta::Zero();
    if (m_intervalHistory.empty())
    {   // initial state
        itvl.type = IntervalType::NORMAL;
        itvl.output_cwnd = itvl.target_cwnd = m_initCwnd;
    }
    else if (m_intervalHistory.back().type == IntervalType::NORMAL)
    {   // create probe interval
        itvl.type = IntervalType::PROBE;
        itvl.output_cwnd = itvl.target_cwnd = CalculateProbeCwnd();
        if (m_intervalHistory.back().target_cwnd < itvl.target_cwnd)
        {
            itvl.output_cwnd = m_intervalHistory.back().target_cwnd;
        }
    }
    else
    {   // create normal interval
        itvl.type = IntervalType::NORMAL;
        itvl.output_cwnd = itvl.target_cwnd = m_bestCwnd;
        if (m_intervalHistory.back().target_cwnd < itvl.target_cwnd)
        {
            itvl.output_cwnd = m_intervalHistory.back().target_cwnd;
        }
    }
    
    itvl.target_cwnd = BoundCwnd(itvl.target_cwnd);
    itvl.output_cwnd = BoundCwnd(itvl.output_cwnd);
    SPDLOG_DEBUG("ccid:{} create new interval:{}", m_id, itvl.ToStr());
    while(m_intervalHistory.size() > kMaxIntervalSize)
    {
        m_intervalHistory.erase(m_intervalHistory.begin());
    }
    m_intervalHistory.push_back(itvl);    
}

uint32_t TestCongestionCtrl::BoundCwnd(uint32_t cwnd)
{
    return std::max(m_minCwnd, cwnd);
}

uint32_t TestCongestionCtrl::CalculateProbeCwnd()
{
    return m_bestCwnd + (1 << m_probeStepPower);
}

TestCongestionCtlConfig::TestCongestionCtlConfig(RenoCongestionCtlConfig& renoconfig)
{
    minCwnd = renoconfig.minCwnd;
};

void Interval::OnDataSent(const InflightPacket& sentpkt)
{
    // interval starts when sentpkt.inflight equals target_cwnd - 1
    if (sentpkt.inflight == target_cwnd - 1 || start_seq != MAX_SEQNUMBER) {
        if (start_seq == MAX_SEQNUMBER)
        {
            start_seq = sentpkt.seq;
        }
        if (end_seq == MAX_SEQNUMBER)
        {
            end_seq = sentpkt.seq;
        }
        if (!first_send_tic.IsInitialized()) {
            first_send_tic = Clock::GetClock()->Now();
        }
        if (sentpkt.seq < start_seq || sentpkt.seq < end_seq)
        {
            SPDLOG_WARN("send reorder packets start_seq:{} end_seq:{} sentpkt.seq:{}",
                        start_seq, end_seq, sentpkt.seq);
        }
        end_seq = std::max(end_seq, sentpkt.seq);
        max_inflight = std::max(max_inflight, sentpkt.inflight);
        ++send_cnt;
    }
}

void Interval::CalculateStatistics()
{
    last_recv_tic = Clock::GetClock()->Now();
    if (last_recv_tic.IsInitialized() && first_recv_tic.IsInitialized()
        && last_recv_tic > first_recv_tic + QuicTime::Delta::FromMilliseconds(1))
    {
        throughput = ack_cnt * 1024.0 * 8 / (last_recv_tic - first_recv_tic).ToMilliseconds();
    } else {
        throughput = ack_cnt * 1024.0 * 8 / (first_recv_tic - first_send_tic).ToMilliseconds();
    }
    throughput = std::max(1u, throughput);
    loss_rate = std::min((float)(send_cnt - ack_cnt) / send_cnt, 0.99f);
    rtt = first_recv_tic - first_send_tic;
    //utility = throughput * (1 - loss_rate) / rtt.ToMilliseconds();
    utility = throughput * (1 - loss_rate) * (1 - loss_rate) * (1 - loss_rate) / std::max(1l, avg_rtt.ToMilliseconds());
    SPDLOG_DEBUG("recv_done:{}", ToStr());
}

void Interval::CheckIfSentDone(uint32_t seq, QuicTime now, uint32_t newly_free)
{
    if (output_cwnd < target_cwnd)
    {
        output_cwnd += newly_free;
        output_cwnd = std::min(output_cwnd, target_cwnd);
    }
    if (start_seq != MAX_SEQNUMBER)
    {
        if (seq >= start_seq && first_recv_tic.IsInitialized() && now >= first_recv_tic + extra_duration) {
            sent_done = true;
        }
    }
}