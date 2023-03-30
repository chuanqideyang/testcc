#include "mpd/download/transportcontroller/transportcontroller.hpp"
#include "sessionstreamcontroller.hpp"

constexpr int kMinAckCntPerInterval = 5;
constexpr float probe_gain[] = {
    1.0/2, 1.0/1.5, 1.0/1.25, 1.0/1.1, 1.0/1.05,
    1.05, 1.1, 1.25, 1.5, 2
};

TestCongestionCtrl::TestCongestionCtrl(const TestCongestionCtlConfig& ccConfig)
{
    m_id = rand();
    SPDLOG_DEBUG("ccid:{}", m_id);
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
    m_intervalHistory.back().CheckIfSentDone(maxLossSeq, now);
}

void TestCongestionCtrl::OnDataRecv(const AckEvent& ackEvent)
{
    QuicTime now = Clock::GetClock()->Now();
    for (auto& itvl : m_intervalHistory)
    {
        if (ackEvent.ackPacket.seq >= itvl.start_seq && ackEvent.ackPacket.seq <= itvl.end_seq)
        {
            if (!itvl.first_recv_tic.IsInitialized()) {
                itvl.first_recv_tic = Clock::GetClock()->Now();
            }
            ++itvl.ack_cnt;
            //itvl.ackTimeInMs.insert(now.ToDebuggingValue() / 1000);
        }
        if (itvl.sent_done && !itvl.recv_done && ackEvent.ackPacket.seq >= itvl.end_seq)
        {
            itvl.recv_done = true;
            itvl.CalculateStatistics();
        }
    }
    m_intervalHistory.back().CheckIfSentDone(ackEvent.ackPacket.seq, now);
}

uint32_t TestCongestionCtrl::GetCWND()
{
    if (!m_intervalHistory.empty()) {
        return m_intervalHistory.back().target_cwnd;
    } else {
        return m_initCwnd;
    }
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
    if (m_intervalHistory.empty())
    {   // initial state
        itvl.target_cwnd = m_initCwnd;
        itvl.base_cwnd = m_initCwnd;
        itvl.probe_gain = 1.0;
    } else
    {   // find the probe interval
        int probe_index = m_intervalHistory.size() - 1;
        for ( ; probe_index >= 0; probe_index--)
        {
            if (m_intervalHistory[probe_index].probe_gain != 1.0)
            {
                break;
            }
        }
        if (probe_index == -1)
        { // create probe interval if no probe interval found
            itvl.target_cwnd = m_initCwnd * 2;
            itvl.base_cwnd = m_initCwnd;
            itvl.probe_gain = 2.0;
        }
        else
        { // probe interval found
            Interval& probe_interval = m_intervalHistory[probe_index];
            Interval& base_interval = m_intervalHistory[probe_index - 1];
            if (probe_interval.recv_done && base_interval.recv_done)
            {   // make decision, first make a base interval and then make a probe interval
                if (probe_interval.max_inflight > base_interval.max_inflight)
                {
                    if (probe_interval.utility > base_interval.utility)
                    {
                        itvl.base_cwnd = 1 + probe_interval.max_inflight;
                        itvl.probe_gain = GetHigherProbeGain(probe_interval.probe_gain);
                    }
                    else
                    {
                        itvl.base_cwnd = 1 + base_interval.max_inflight;
                        itvl.probe_gain = GetLowerProbeGain(1.0);
                    }
                }
                else
                {
                    if (probe_interval.utility > base_interval.utility)
                    {
                        itvl.base_cwnd = 1 + probe_interval.max_inflight;
                        itvl.probe_gain = GetLowerProbeGain(probe_interval.probe_gain);
                    }
                    else
                    {
                        itvl.base_cwnd = 1 + base_interval.max_inflight;
                        itvl.probe_gain = GetHigherProbeGain(1.0);
                    }
                }
                float delta_cwnd = itvl.base_cwnd * (itvl.probe_gain - 1);
                if (delta_cwnd < 0) {
                    delta_cwnd = std::min(-1.0f, delta_cwnd);
                }
                if (delta_cwnd > 0) {
                    delta_cwnd = std::max(1.0f, delta_cwnd);
                }
                itvl.target_cwnd = itvl.base_cwnd + (int)delta_cwnd;

                if (m_intervalHistory.back().target_cwnd != itvl.base_cwnd
                    || m_intervalHistory.back().probe_gain != 1.0)
                {   // create a new base interval
                    itvl.target_cwnd = itvl.base_cwnd;
                    itvl.probe_gain = 1.0;
                    SPDLOG_DEBUG("ccid:{} base_interval:{} probe_interval:{} new base interval:{}", m_id, base_interval.ToStr(), probe_interval.ToStr(), itvl.ToStr());
                }
                else
                {   // create a new probe interval and erase history             
                    SPDLOG_DEBUG("ccid:{} base_interval:{} probe_interval:{} new probe interval:{}", m_id, base_interval.ToStr(), probe_interval.ToStr(), itvl.ToStr());
                    m_intervalHistory.erase(m_intervalHistory.begin(), m_intervalHistory.begin() + probe_index + 1);
                }
            }
            else
            { // can not make decision, don't create probe interval
                itvl.target_cwnd = probe_interval.base_cwnd;
                itvl.base_cwnd = probe_interval.base_cwnd;
                itvl.probe_gain = 1.0;
            }
        }
    }
    itvl.base_cwnd = std::max(m_minCwnd, itvl.base_cwnd);
    itvl.target_cwnd = std::max(m_minCwnd, itvl.target_cwnd);
    SPDLOG_DEBUG("ccid:{} create new interval:{}", m_id, itvl.ToStr());
    m_intervalHistory.push_back(itvl);    
}

float TestCongestionCtrl::GetHigherProbeGain(float gain)
{
    int len = sizeof(probe_gain) / sizeof(probe_gain[0]);
    int i = 0;
    for (; i < len; i++)
    {
        if (probe_gain[i] > gain)
        {
            return probe_gain[i];
        }
    }
    return probe_gain[len - 1];
}

float TestCongestionCtrl::GetLowerProbeGain(float gain)
{
    int len = sizeof(probe_gain) / sizeof(probe_gain[0]);
    int i = len - 1;
    for (; i >= 0; i--)
    {
        if (probe_gain[i] < gain)
        {
            return probe_gain[i];
        }
    }
    return probe_gain[0];
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
    //if (last_recv_tic.IsInitialized() && first_recv_tic.IsInitialized()
    //    && last_recv_tic > first_recv_tic + QuicTime::Delta::FromMilliseconds(1))
    //{
    //    throughput = ack_cnt * 1024.0 * 8 / (last_recv_tic - first_recv_tic).ToMilliseconds();
    //} else {
    //    throughput = ack_cnt * 1024.0 * 8 / (first_recv_tic - first_send_tic).ToMilliseconds();
    //}
    throughput = ack_cnt * 1024.0 * 8 / (last_recv_tic - first_send_tic).ToMilliseconds();
    throughput = std::max(1u, throughput);
    loss_rate = std::min((float)(send_cnt - ack_cnt) / send_cnt, 0.99f);
    rtt = first_recv_tic - first_send_tic;
    utility = throughput * (1 - loss_rate) / rtt.ToMilliseconds();
    SPDLOG_DEBUG("recv_done:{}", ToStr());
}

void Interval::CheckIfSentDone(uint32_t seq, QuicTime now)
{
    if (start_seq != MAX_SEQNUMBER)
    {
        if (seq >= start_seq && now >= first_send_tic + extra_duration) {
            sent_done = true;
        }
    }
}