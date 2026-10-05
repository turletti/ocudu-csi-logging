
      // CSI logging (CSI CSV format v3.1, see srs_csi_rb_logger.h), from the TA-compensated LSE. With several SRS
      // ports, the LSE of one port still contains the other ports (they only cancel out over the whole band), so only
      // single-port SRS is logged.
      if (csi_log::srs_csi_rb_logger* csi_rb_log = csi_log::srs_csi_rb_logger::get(); csi_rb_log != nullptr) {
        if (nof_tx_antenna_ports == 1) {
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
                                 comb_size);
        } else {
          csi_rb_log->note_multiport_skipped();
        }
      }
