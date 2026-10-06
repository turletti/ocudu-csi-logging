
      // CSI logging (CSI CSV format v3.1, see srs_csi_rb_logger.h), from the TA-compensated LSE. With N SRS ports on
      // the same comb (cyclic shifts n_cs_max / N apart), the LSE of one port still contains the other ports with a
      // phase that turns by 2 pi (q - p) / N per pilot: the mean of N consecutive pilots cancels them. With four
      // ports on interleaved combs, two ports share each comb.
      if (csi_log::srs_csi_rb_logger* csi_rb_log = csi_log::srs_csi_rb_logger::get(); csi_rb_log != nullptr) {
        unsigned csi_group = (nof_tx_antenna_ports == 1) ? 1U : (interleaved_pilots ? 2U : nof_tx_antenna_ports);
        // The RNTI comes from the FAPI SRS PDU (srs_context); 0 when the PDU carries no context.
        uint16_t csi_rnti = config.context.has_value() ? static_cast<uint16_t>(config.context->get_rnti()) : 0;
        csi_rb_log->log_pilots(config.slot.sfn(),
                               static_cast<uint16_t>(config.slot.slot_index()),
                               csi_rnti,
                               static_cast<uint8_t>(config.ports[i_rx_port]),
                               static_cast<uint8_t>(i_antenna_port),
                               static_cast<uint8_t>(nof_rx_ports),
                               static_cast<uint8_t>(nof_tx_antenna_ports),
                               mean_lse.data(),
                               mean_lse.size(),
                               info.mapping_initial_subcarrier,
                               comb_size,
                               csi_group);
      }
